/*
 * WUW-CAM S3 — ESP32-S3-WROOM camera firmware
 * ─────────────────────────────────────────────────────────────────
 *  WiFi AP   : wuw / wuwuwuwu until the owner sets their own (stored in NVS, key "appass")
 *  Web UI    : http://192.168.4.1        (port 80)
 *  MJPEG     : http://192.168.4.1:81/stream   (desktop bonus)
 *  Photo     : sensor-native maximum → SD card as /IMG_xxxx.jpg
 *  SD Format : FAT32, SDMMC 1-bit on CLK=39 CMD=38 D0=40
 *
 *  Same architecture as the ESP32 build (see esp32cam-webserver-design):
 *    · esp_http_server ONLY — no AsyncTCP
 *    · JPEG sent straight from the framebuffer — zero heap copy
 *    · sequential /jpg snapshot polling for the UI (iOS-safe), MJPEG on :81
 *
 *  S3 perks used here: native USB (auto-reset flashing — no IO0 jumper!),
 *  8 MB octal PSRAM, camera does NOT sit on a strapping pin.
 * ─────────────────────────────────────────────────────────────────
 */

#include "Arduino.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "SD_MMC.h"
#include <Preferences.h>
#include "pasar.h"
#include "WuwEventBridge.h"
#include "panel.h"
#include "link.h"
#include "FS.h"
#include "WiFi.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <lwip/sockets.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "video_writer.h"
#include "jpeg_orient.h"
#include "http_range.h"
#include "ap_pass.h"
#include "ui_art.h"
#include <stdarg.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "esp_ota_ops.h"
#include "esp_idf_version.h"

// ═══════════════════════════════════════════════════════════════════════
//  PIN DEFINITIONS — ESP32-S3-WROOM CAM (Freenove map)
// ═══════════════════════════════════════════════════════════════════════
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    15
#define SIOD_GPIO_NUM     4   // SCCB SDA
#define SIOC_GPIO_NUM     5   // SCCB SCL
#define Y9_GPIO_NUM      16   // D7
#define Y8_GPIO_NUM      17   // D6
#define Y7_GPIO_NUM      18   // D5
#define Y6_GPIO_NUM      12   // D4
#define Y5_GPIO_NUM      10   // D3
#define Y4_GPIO_NUM       8   // D2
#define Y3_GPIO_NUM       9   // D1
#define Y2_GPIO_NUM      11   // D0
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM    13

// SDMMC 1-bit (S3 routes SDMMC through the GPIO matrix — pins must be set)
#define SD_CLK_PIN       39
#define SD_CMD_PIN       38
#define SD_D0_PIN        40

// Onboard LED (no real flash LED on this board — this is the status LED;
// if nothing lights when you toggle Flash, tell me and we change the pin)

// ═══════════════════════════════════════════════════════════════════════
//  WIFI AP
// ═══════════════════════════════════════════════════════════════════════
#define FW_VERSION "1.0.0"
#ifdef WUW_SENSOR_OV3660
#define FW_MODEL   "WUW CAM S3 OV3660"
#else
#define FW_MODEL   "WUW CAM S3"
#endif

const char* AP_SSID = "wuw";

// For production runs, -DUNIQUE_SSID appends a MAC suffix so units in the same
// room never collide (wuw-A4F1). Left off by default so the SSID stays "wuw".
static char apName[24];

static DNSServer dnsServer;   // captive portal: every lookup resolves to us

/* ── OTA ───────────────────────────────────────────────────────────────
   The bootloader here is built WITHOUT rollback support, so a firmware
   that crashes before it finishes booting would loop forever. This is the
   software substitute: count boot attempts in NVS, and if three in a row
   never reach READY, switch back to the other OTA slot. It costs one NVS
   write per boot and it is the difference between a bad release being an
   inconvenience and being a brick. */
static void ev(const char* fmt, ...);   // event ring, defined below
static Preferences prefs;
static String  staSsid, staPass, otaUrl;
static bool    staOn = false;
static String  otaLatest, otaNotes;
#define BOOT_TRY_LIMIT 3

static void bootGuardBegin() {
  prefs.begin("wuw", false);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  otaUrl  = prefs.getString("otaurl",
            "https://ritualpenang.online/wuwcam/manifest.json");
  uint8_t tries = prefs.getUChar("boottry", 0) + 1;
  prefs.putUChar("boottry", tries);
  if (tries > BOOT_TRY_LIMIT) {
    const esp_partition_t* other = esp_ota_get_next_update_partition(NULL);
    Serial.printf("[OTA] %u failed boots — reverting to previous firmware\n", tries);
    prefs.putUChar("boottry", 0);
    if (other && esp_ota_set_boot_partition(other) == ESP_OK) {
      prefs.end(); delay(200); ESP.restart();
    }
  }
}
static void bootGuardOK() {          // called once everything is actually up
  prefs.putUChar("boottry", 0);
}

/* ── The camera's WiFi password ───────────────────────────────────────────
 * The factory password is printed in the manual, so anyone who has read it can join any camera
 * still using it and watch, record from and download everything on it. Each owner can set their
 * own from the page; it is stored in NVS, applied on every boot and live when changed, and is never
 * sent back to the page (the page only learns whether one is set). If it is forgotten, holding the
 * shutter button while powering on for 8 seconds restores the factory password (apRecoveryCheck).
 * The access-point name is not a secret and does not change. */
static char     apPass[AP_PASS_MAX + 1] = AP_PASS_FACTORY;
static bool     apPassCustom = false;
static uint32_t apApplyAt = 0;            // millis() at which a changed password goes live; 0 = nothing pending

static void apPassLoad() {
  String saved = prefs.getString("appass", "");
  if (apPassCheck(saved.c_str(), saved.length()) == AP_PASS_OK) {
    snprintf(apPass, sizeof(apPass), "%s", saved.c_str());
    apPassCustom = true;
  } else {
    snprintf(apPass, sizeof(apPass), "%s", AP_PASS_FACTORY);
    apPassCustom = false;
  }
}

// AP stays up so the camera keeps working; STA is only for reaching the net.
static bool staConnect(uint32_t waitMs = 12000) {
  if (!staSsid.length()) return false;
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) delay(200);
  staOn = (WiFi.status() == WL_CONNECTED);
  ev("STA %s %s", staSsid.c_str(), staOn ? WiFi.localIP().toString().c_str() : "FAILED");
  return staOn;
}

/* ── Upstream WiFi: one set of functions for everything that needs it ───────
 * The web page, the on-device WiFi screen and the reconnect watchdog all go
 * through these, so there is one place that knows how a join is started and
 * one place that keeps the link alive.
 *
 * That second job used to belong to the chat module, as a side effect: its
 * task re-issued WiFi.begin() whenever the link dropped. Nothing else did, and
 * staOn was only refreshed inside staConnect() and the /net handler -- so a
 * dropped link was never noticed and a failed join was never retried. Taking
 * chat out without putting this somewhere would have silently removed
 * reconnection from the whole device.
 *
 * Joins are NON-BLOCKING: a handler or a touch must not sit in an eight-second
 * association loop. Callers poll staState(). */
static uint32_t staJoinAt = 0, staLastTry = 0;

static void staJoin(const char* ssid, const char* pass) {
  String s = ssid ? ssid : "", p = pass ? pass : "";    // copies: callers may pass our own c_str()
  if (!s.length()) return;
  staSsid = s; staPass = p;
  prefs.putString("ssid", staSsid);
  prefs.putString("pass", staPass);
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);       // drop the old association, keep the access point
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  staOn = false; staJoinAt = staLastTry = millis();
  ev("STA join %s", staSsid.c_str());
}

__attribute__((unused)) static void staForget() {   // only the panel build calls it
  staSsid = ""; staPass = "";
  prefs.remove("ssid"); prefs.remove("pass");
  WiFi.disconnect(false, true);        // also erase what the radio itself remembers
  staOn = false;
  ev("STA forgotten");
}

/* Once a second from loop(). Keeps staOn truthful and retries a dropped or
   failed join -- slowly, because in AP+STA mode every attempt makes the radio
   scan, and a scan briefly interrupts everyone on the access point. */
static void staTick() {
  static uint32_t last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != staOn) {
    staOn = up;
    ev("STA %s", up ? (String("up ") + WiFi.localIP().toString()).c_str() : "down");
  }
  if (!staOn && staSsid.length() && millis() - staLastTry > 30000) {
    staLastTry = millis();
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }
}

// ═══════════════════════════════════════════════════════════════════════
//  GLOBAL STATE
// ═══════════════════════════════════════════════════════════════════════
static bool    nightVision = false;
static bool    sdReady     = false;
static bool    camOk       = false;   // false → AP + UI still come up, camera disabled
static uint8_t streamRes   = FRAMESIZE_SVGA;
static void lightModeChanged();                // re-derives the shutter labels for the mode just set
#ifdef WUW_SENSOR_OV3660
static uint8_t photoRes    = FRAMESIZE_QXGA;    // 2048x1536 - OV3660 3MP
#else
static uint8_t photoRes    = FRAMESIZE_QSXGA;   // 2560×1920 — OV5640 5MP
#endif
/* Neutral exposure is the default: +2 brightness and +1 AE level lifted every scene and washed
   the highlights toward a pink cast. Both stay user-adjustable; the stored value only wins once
   the person has actually changed it (see camToneMigrate). */
static int8_t  camBrightness = 0;
static int8_t  camAeLevel    = 0;
static bool    camHMirror = true;     // this module is physically mirrored
static bool    camVFlip = false;
/* How the picture is turned when shown, in clockwise quarter turns. The sensor is mounted so a
   portrait scene arrives sideways, which the TFT has always corrected (VIEW ROTATE, default 1).
   The phone page, saved photos (EXIF) and recordings (track matrix) now use the same number, kept
   in the same NVS keys the panel uses so the glass and the phone can never disagree. */
static uint16_t camSensorPid = 0;
#ifdef WUW_SENSOR_OV3660
static uint8_t  camMaxRes = FRAMESIZE_QXGA;
#else
static uint8_t  camMaxRes = FRAMESIZE_QSXGA;
#endif
static const char* camSensorName = "unknown";
static int     photoCount  = 0;
/* The name this camera answers to: reported in the room, shown in the UI,
   persisted so it survives a reflash. Declared up here because handlers far
   below reference it and C++ has no forward declaration for a definition. */
static char    devName[24] = "WUW-01";

/* qGet hands back the raw query substring. Percent escapes therefore arrive
   literally -- a space typed as %20 became the characters "20" once the
   sanitiser dropped the %. Decode before sanitising, never after. */
static void urlDecode(const char* in, char* out, size_t outN) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 1 < outN; i++) {
    if (in[i] == '%' && isxdigit((unsigned char)in[i+1]) &&
                        isxdigit((unsigned char)in[i+2])) {
      char hex[3] = { in[i+1], in[i+2], 0 };
      out[j++] = (char)strtol(hex, nullptr, 16);
      i += 2;
    } else if (in[i] == '+') out[j++] = ' ';
    else out[j++] = in[i];
  }
  out[j] = 0;
}

static int     recFileNum  = 0;
static int     streamQ     = 12;   // live JPEG quality (lower = better)
static int     photoQ      = 4;    // stills get near-max quality

static bool validFrameSize(uint8_t value) {
  switch (value) {
    case FRAMESIZE_QVGA: case FRAMESIZE_VGA: case FRAMESIZE_SVGA:
    case FRAMESIZE_XGA: case FRAMESIZE_HD: case FRAMESIZE_SXGA:
    case FRAMESIZE_UXGA: case FRAMESIZE_FHD: case FRAMESIZE_QXGA:
    case FRAMESIZE_QHD: case FRAMESIZE_QSXGA:
      return value <= camMaxRes;
    default: return false;
  }
}

#define EV_N   14
#define EV_LEN  92
static char     evBuf[EV_N][EV_LEN];
static uint8_t  evHead = 0, evUsed = 0;

static void ev(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vsnprintf(evBuf[evHead], EV_LEN, fmt, ap);
  va_end(ap);
  Serial.printf("[EV] %lu %s\n", (unsigned long)(millis()/1000), evBuf[evHead]);
  evHead = (evHead + 1) % EV_N;
  if (evUsed < EV_N) evUsed++;
}

static void evDump() {
  if (!evUsed) { Serial.println("[EVLOG] (nothing yet)"); return; }
  Serial.printf("[EVLOG] last %u events:\n", evUsed);
  for (uint8_t i = 0; i < evUsed; i++) {
    uint8_t idx = (evHead + EV_N - evUsed + i) % EV_N;
    Serial.printf("   %2u. %s\n", i + 1, evBuf[idx]);
  }
}

static httpd_handle_t webHttpd    = NULL;
static httpd_handle_t streamHttpd = NULL;

/* Current view rotation, straight from NVS. The panel's CAMERA menu writes the same two keys
   when VIEW ROTATE is tapped, so reading them fresh (an in-RAM read, NVS caches its pages) keeps
   the phone page, saved photos and recordings in step with the glass. Until a choice is made the
   camera is "portrait-source footage turned clockwise" (1), exactly what the panel defaults to. */
static uint8_t camViewRotNow() {
  if (prefs.getUChar("viewrotv", 0) < 1) return 1;
  return prefs.getUChar("viewrot", 1) & 3;
}

static void camSetViewRot(uint8_t turns) {
  turns &= 3;
  prefs.putUChar("viewrot", turns);
  prefs.putUChar("viewrotv", 1);
  rec_set_rotation(turns);
#ifdef WUW_PANEL
  panelSetViewRot(turns);
#endif
}

/* One-time tone reset. Earlier firmware shipped +2 brightness and +1 exposure compensation,
   which pushed every scene bright and washed the highlights toward pink; whatever number a unit
   had stored may simply have been that default carried along. The first boot of this firmware
   drops both so the sensor starts neutral. They stay adjustable and are remembered from then on. */
static void camToneMigrate() {
  if (prefs.getUChar("tonev", 0) >= 1) return;
  prefs.remove("bright");
  prefs.remove("aelevel");
  prefs.putUChar("tonev", 1);
  Serial.println("[CAM] tone defaults reset to neutral (one-time)");
}

// ═══════════════════════════════════════════════════════════════════════
//  CAMERA INIT
// ═══════════════════════════════════════════════════════════════════════
static bool initCamera() {
  camera_config_t cfg = {};          // zero-init: untouched fields stay sane
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
  /* 24 MHz, up from 20. Measured on this board (rgbbench sensorSweep): frame
     rate scales linearly with XCLK and stops gaining at 24 MHz --
        XCLK   10     16     20     24     30     40
        fps    11.1   fails  22.2   28.4   28.3   28.3    (QVGA..SVGA)
     so 20 MHz left about 28% of the sensor's frame rate unused for no reason
     except that it was the example's value. 24 MHz is also the OV5640's
     recommended clock. 16 MHz does not initialise at all. */
  cfg.xclk_freq_hz = 24000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.grab_mode    = CAMERA_GRAB_LATEST;   // stream + recorder never stall each other
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;

  if (psramFound()) {
#ifdef WUW_SENSOR_OV3660
    cfg.frame_size   = FRAMESIZE_QXGA;     // allocate at the OV3660 native maximum
#else
    cfg.frame_size   = FRAMESIZE_QSXGA;    // alloc at 5MP so any framesize fits later
#endif
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 2;
  } else {
    cfg.frame_size   = FRAMESIZE_SVGA;
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 1;
    cfg.fb_location  = CAMERA_FB_IN_DRAM;
    photoRes         = FRAMESIZE_UXGA;
    Serial.println("[CAM] No PSRAM — photo capped at UXGA");
  }

  // Retry loop kept from the ESP32 build; PWDN is not wired on this board,
  // so between attempts we can only wait for the rail to settle.
  esp_err_t err = ESP_FAIL;
  for (int attempt = 1; attempt <= 3; attempt++) {
    err = esp_camera_init(&cfg);
    if (err == ESP_OK) break;
    Serial.printf("[CAM] attempt %d/3 failed: 0x%x%s\n", attempt, err,
                  err == ESP_ERR_NOT_FOUND ? " (sensor did not answer on SCCB)" : "");
    esp_camera_deinit();
    delay(500);
  }
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init FAILED after 3 attempts: 0x%x\n", err);
    printf("[CAM] sensor did not answer on SCCB (GPIO %d/%d) - check the "
           "ribbon latch and that nothing else is on those pins\n",
           SIOD_GPIO_NUM, SIOC_GPIO_NUM);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  camSensorPid = s->id.PID;
  camera_sensor_info_t* sensorInfo = esp_camera_sensor_get_info(&s->id);
  if (sensorInfo) {
    camSensorName = sensorInfo->name;
    camMaxRes = (uint8_t)sensorInfo->max_size;
  }
  Serial.printf("[CAM] sensor PID: 0x%04x (%s), max frame enum %u\n",
                camSensorPid, camSensorName, camMaxRes);

  // ── Sensor defaults, shared by the web UI and the panel OS ──
  uint8_t savedStream = prefs.getUChar("streamres", streamRes);
  uint8_t savedPhoto  = prefs.getUChar("photores", photoRes);
  if (validFrameSize(savedStream)) streamRes = savedStream;
  if (validFrameSize(savedPhoto))  photoRes = savedPhoto;
  if (photoRes > camMaxRes) {
    photoRes = camMaxRes;
    prefs.putUChar("photores", photoRes);
  }
  streamQ = constrain(prefs.getInt("streamq", streamQ), 8, 45);
  photoQ  = constrain(prefs.getInt("photoq", photoQ), 4, 20);
  camToneMigrate();
  camBrightness = constrain(prefs.getInt("bright", camBrightness), -2, 2);
  camAeLevel    = constrain(prefs.getInt("aelevel", camAeLevel), -2, 2);
  rec_set_rotation(camViewRotNow());
  camHMirror = prefs.getBool("hmirror", camHMirror);
  camVFlip   = prefs.getBool("vflip", camVFlip);
  s->set_framesize(s, (framesize_t)streamRes);
  s->set_quality(s, streamQ);
  s->set_brightness(s, camBrightness);
  s->set_contrast(s, 0);
  s->set_sharpness(s, 0);
  s->set_special_effect(s, 0);                 // before saturation: it resets the colour registers
  s->set_saturation(s, constrain(prefs.getInt("sat", 0), -2, 2));
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, constrain(prefs.getInt("wbmode", 0), 0, 4));
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_ae_level(s, camAeLevel);
  s->set_aec_value(s, 300);
  s->set_gain_ctrl(s, 1);
  s->set_agc_gain(s, 0);
  s->set_gainceiling(s, GAINCEILING_8X);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
  s->set_hmirror(s, camHMirror);
  s->set_vflip(s, camVFlip);
  s->set_dcw(s, 1);
  s->set_colorbar(s, 0);

  Serial.printf("[CAM] OK — PSRAM: %s\n", psramFound() ? "YES" : "NO");
  printf("[CAM] sensor 0x%02x OK\n", s->id.PID);
  return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  SD CARD INIT — SDMMC 1-bit via GPIO matrix
// ═══════════════════════════════════════════════════════════════════════
// Continue numbering from what is already on the card. Without this the
// counters restart at 0 each boot and quietly overwrite earlier captures.
static void scanExisting() {
  File root = SD_MMC.open("/");
  if (!root) return;
  File f;
  int n = 0;
  while ((f = root.openNextFile()) && n < 2000) {
    const char* nm = f.name();
    const char* slash = strrchr(nm, '/');
    if (slash) nm = slash + 1;
    int v;
    if (sscanf(nm, "IMG_%d.jpg", &v) == 1 && v >= photoCount) photoCount = v + 1;
    if (sscanf(nm, "VID_%d.mov", &v) == 1 && v >= recFileNum) recFileNum = v + 1;
    if (sscanf(nm, "VID_%d.mp4", &v) == 1 && v >= recFileNum) recFileNum = v + 1;
    n++;
    f.close();
  }
  root.close();
#ifdef EXHIBITION_MODE
  /* Exhibition images live in per-session folders, so a root scan alone would
     restart numbering every session and overwrite the previous one's frames.
     The high-water mark is also persisted, and the larger of the two wins --
     so numbering survives a reboot, a reformat of the root, and a card that
     was swapped mid-exhibition. */
  {
    Preferences hw; hw.begin("pasar", false);
    int keep = (int)hw.getUInt("imgmax", 0);
    if (keep > photoCount) photoCount = keep;
    hw.putUInt("imgmax", (uint32_t)photoCount);
    hw.end();
  }
#endif
  Serial.printf("[SD] %d files on card - next IMG_%d / VID_%d\n",
                n, photoCount, recFileNum);
}

static bool initSD() {
  if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN)) {
    Serial.println("[SD] setPins FAILED");
    return false;
  }
  if (!SD_MMC.begin("/sdcard", true)) {   // true = 1-bit mode
    Serial.println("[SD] Mount FAILED");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[SD] No card");
    return false;
  }
  Serial.printf("[SD] OK — %llu GB\n", SD_MMC.cardSize() / (1024ULL * 1024 * 1024));
  scanExisting();
  return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  NIGHT VISION
// ═══════════════════════════════════════════════════════════════════════
static void applyNightVision(bool en) {
  sensor_t* s = esp_camera_sensor_get();
  if (en) {
    s->set_gain_ctrl(s, 0);
    s->set_agc_gain(s, 30);
    s->set_gainceiling(s, GAINCEILING_128X);
    s->set_exposure_ctrl(s, 0);
    s->set_aec_value(s, 1200);
    s->set_aec2(s, 0);
    s->set_brightness(s, 2);
    s->set_contrast(s, 2);
    s->set_saturation(s, -2);
    s->set_lenc(s, 0);
    s->set_raw_gma(s, 0);
    s->set_bpc(s, 0);
    s->set_wpc(s, 0);
  } else {
    s->set_gain_ctrl(s, 1);
    s->set_gainceiling(s, GAINCEILING_8X);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_ae_level(s, camAeLevel);
    s->set_brightness(s, camBrightness);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_lenc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
    s->set_hmirror(s, camHMirror);
    s->set_vflip(s, camVFlip);
  }
}

// ═══════════════════════════════════════════════════════════════════════
//  PHOTO CAPTURE → SD  (5MP QSXGA)
// ═══════════════════════════════════════════════════════════════════════
/* Writes through to an SD file; the sink jpegWriteOriented() streams a photo into, so a turned
   photo gets its EXIF orientation spliced in without a second copy of the frame in memory. */
static bool fileSink(void* ctx, const uint8_t* data, size_t count) {
  return static_cast<File*>(ctx)->write(data, count) == count;
}
struct MemSink { uint8_t* p; size_t cap; size_t n; };
static bool memSink(void* ctx, const uint8_t* data, size_t count) {
  MemSink* m = static_cast<MemSink*>(ctx);
  if (m->n + count > m->cap) return false;
  memcpy(m->p + m->n, data, count);
  m->n += count;
  return true;
}
static char lastShotBase[64] = "";   // path stem of the most recent frame
static uint32_t lastShutterLagMs = 0;    // measured, not assumed

static String capturePhoto() {
  sensor_t* s = esp_camera_sensor_get();
  framesize_t saved = (framesize_t)streamRes;
  uint32_t t0 = millis();

  /* ── Shutter lag ────────────────────────────────────────────────────────
     Changing frame size costs half a second: the sensor re-times, and the
     first frame or two after the switch are exposed wrong. That is the price
     of a 5 MP still and it is worth paying -- but only when the sensor is
     actually somewhere else. When the photo size already matches what the
     viewfinder is running, there is nothing to switch to and nothing to
     settle, and the whole 500 ms is pure waste.

     So it is skipped. Set photoRes = streamRes and the shutter fires on the
     frame the sensor already has. That is the difference between a camera
     you aim and a camera you point. */
  const bool switching = ((framesize_t)photoRes != saved);
  camera_fb_t* fb = NULL;
  if (switching) {
    s->set_framesize(s, (framesize_t)photoRes);
    delay(400);                                 // 5MP mode switch settle
    fb = esp_camera_fb_get();                   // discard stale frame
    if (fb) { esp_camera_fb_return(fb); fb = NULL; }
    delay(100);
  }

  fb = esp_camera_fb_get();
  lastShutterLagMs = millis() - t0;
  String result = "ERR_CAPTURE";
  if (fb) {
    if (sdReady) {
      char base[48], path[64];
#ifdef EXHIBITION_MODE
      SD_MMC.mkdir(pasarSessionDir());
      snprintf(base, sizeof(base), "%s/IMG_%04d", pasarSessionDir(), photoCount);
#else
      snprintf(base, sizeof(base), "/IMG_%d", photoCount);
#endif
      snprintf(path, sizeof(path), "%s.jpg", base);
      File f = SD_MMC.open(path, FILE_WRITE);
      if (f) {
        size_t wrote = jpegWriteOriented(fileSink, &f, fb->buf, fb->len, camViewRotNow());
        f.close();
        if (!wrote) {                    // card full or pulled mid-write
          SD_MMC.remove(path);           // never leave a truncated frame behind
          result = "ERR_SD_FULL";
        } else {
          lastShotBase[0] = 0;
          snprintf(lastShotBase, sizeof(lastShotBase), "%s", base);
          photoCount++;
#ifdef EXHIBITION_MODE
          { Preferences hw; hw.begin("pasar", false);
            hw.putUInt("imgmax", (uint32_t)photoCount); hw.end(); }
#endif
          result = String(path) + " " + String(fb->len / 1024) + "KB";
        }
      } else result = "ERR_FILE_OPEN";
    } else result = "ERR_NO_SD";
    esp_camera_fb_return(fb);
  }

  if (switching) s->set_framesize(s, saved);
  return result;
}

// ═══════════════════════════════════════════════════════════════════════
//  WEB UI  — minimal embedded HTML/CSS/JS
// ═══════════════════════════════════════════════════════════════════════
static const char INDEX_HTML[] = R"rawliteral(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<meta name="apple-mobile-web-app-capable" content="yes">
<title>WUWCAM S3</title>
<style>
:root{
  --void:#06070a; --ink:#171b21; --edge:#2a313a;
  --bone:#c6cdd3; --dim:#78828c;
  --sig:#79c2a4;        /* jade  — primary accent   */
  --sig2:#a493e0;       /* lavender — live signal   */
  --blood:#e8788f;      /* blush red — recording    */
  --rim:#7a8794;        /* edge catch-light         */
}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--void);color:var(--bone);font:13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;
height:100vh;height:100dvh;display:flex;flex-direction:column;overflow:hidden;-webkit-tap-highlight-color:transparent;
letter-spacing:.02em}
/* No vignette or grain overlay on the page: nothing is drawn over the camera picture. */
header{background:linear-gradient(180deg,#0a0c14,#06070c);border-bottom:1px solid var(--edge);
padding:7px 11px;display:flex;justify-content:space-between;align-items:center;flex-shrink:0;font-size:11px}
.logo{color:var(--bone);font-size:13px;font-weight:600;letter-spacing:.34em;display:flex;align-items:center;gap:7px}
.logo .mark{color:var(--sig);font-size:14px;display:inline-block;animation:breathe 6s ease-in-out infinite}
@keyframes breathe{0%,100%{opacity:.55;transform:scale(1)}50%{opacity:1;transform:scale(1.12)}}
.hstat{color:var(--dim);font-size:10px;letter-spacing:.1em}
.hstat b{color:var(--sig2);font-weight:400}
.main{flex:1;display:flex;overflow:hidden;min-height:0}
.sv{flex:1;background:radial-gradient(ellipse at 50% 50%,#0b0d15,#040509);position:relative;touch-action:none;
display:flex;align-items:center;justify-content:center;overflow:hidden}
#si{max-width:100%;max-height:100%;display:block;object-fit:contain}
#si.dim{opacity:0}
#gc{position:absolute;top:50%;left:50%;-webkit-transform:translate(-50%,-50%);transform:translate(-50%,-50%);max-width:100%;max-height:100%;display:none}
#gc.on{display:block}
.cnr{position:absolute;width:18px;height:18px;border-color:var(--sig);border-style:solid;opacity:.45;z-index:6}
.cnr.tl{top:10px;left:10px;border-width:1px 0 0 1px}
.cnr.tr{top:10px;right:10px;border-width:1px 1px 0 0}
.cnr.bl{bottom:10px;left:10px;border-width:0 0 1px 1px}
.cnr.br{bottom:10px;right:10px;border-width:0 1px 1px 0}
#se{display:none;position:absolute;color:var(--onwell);font-size:11px;text-align:center;padding:10px;z-index:7;letter-spacing:.16em}
#se.on{display:block}
#rb{display:none;position:absolute;top:9px;left:50%;transform:translateX(-50%);
background:rgba(255,58,94,.16);border:1px solid var(--blood);color:var(--blood);
font-size:10px;padding:2px 10px;letter-spacing:.2em;z-index:8}
#rb.on{display:block;animation:pulse 1.6s ease-in-out infinite}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.45}}
#fps{position:absolute;bottom:8px;left:12px;color:var(--sig);font-size:10px;opacity:.5;z-index:8;letter-spacing:.12em}
#phase{position:absolute;bottom:8px;right:12px;color:var(--dim);font-size:10px;z-index:8;letter-spacing:.2em}
/* sidebar */
.sb{width:236px;background:linear-gradient(180deg,#07080e,#05060a);border-left:1px solid var(--edge);
overflow-y:auto;padding:8px;flex-shrink:0;display:flex;flex-direction:column;gap:5px;
-webkit-overflow-scrolling:touch;overscroll-behavior:contain}
.sb::-webkit-scrollbar{width:2px}
.sb::-webkit-scrollbar-thumb{background:var(--edge)}
/* native accordion — no JS, keeps the wall of controls collapsed */
details{border:1px solid var(--edge);background:rgba(255,255,255,.012)}
details[open]{background:rgba(255,255,255,.022)}
summary{list-style:none;cursor:pointer;padding:7px 9px;font-size:10px;letter-spacing:.22em;
color:var(--sig);display:flex;align-items:center;gap:7px;user-select:none;touch-action:manipulation}
summary::-webkit-details-marker{display:none}
summary::before{content:'\25C7';font-size:9px;opacity:.7;transition:transform .25s ease}
details[open] summary::before{content:'\25C6';transform:rotate(90deg)}
.dbody{padding:4px 9px 10px;display:flex;flex-direction:column;gap:5px}
.row{display:flex;gap:5px}
button{background:rgba(255,255,255,.03);border:1px solid var(--edge);color:var(--bone);
font:11px ui-monospace,Menlo,monospace;padding:8px 5px;cursor:pointer;flex:1;
touch-action:manipulation;letter-spacing:.1em;transition:border-color .2s,color .2s,background .2s}
button:active{transform:scale(.97)}
button.on{border-color:var(--sig);color:var(--sig);background:rgba(232,182,97,.09)}
button.ron{border-color:var(--blood);color:var(--blood);background:rgba(255,58,94,.09)}
.lbl{font-size:9px;color:var(--dim);letter-spacing:.14em;margin-bottom:1px}
select{width:100%;background:var(--ink);color:var(--bone);border:1px solid var(--edge);
font:11px ui-monospace,Menlo,monospace;padding:6px;-webkit-appearance:none;border-radius:0;letter-spacing:.06em}
select:focus{outline:none;border-color:var(--sig)}
.sl{display:grid;grid-template-columns:56px 1fr 26px;gap:5px;align-items:center;font-size:9px;color:var(--dim);letter-spacing:.08em}
.sl span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sl input[type=range]{accent-color:var(--sig);width:100%;height:16px}
.sl span:last-child{color:var(--sig);text-align:right}
.ck{display:flex;flex-wrap:wrap;gap:9px}
.ck label{font-size:9px;color:var(--dim);display:flex;align-items:center;gap:4px;cursor:pointer;touch-action:manipulation}
.ck input{accent-color:var(--sig);width:15px;height:15px}
.cbar{padding:7px 8px;border-top:1px solid var(--edge);display:flex;gap:5px;flex-shrink:0;flex-wrap:wrap;
background:linear-gradient(180deg,#06070c,#030407)}
.cbar button{padding:11px 6px;font-size:11px;flex:1;min-width:74px}
input[type=color]{-webkit-appearance:none;appearance:none;border:1px solid var(--edge);
background:transparent;width:100%;height:28px;padding:2px;cursor:pointer;flex:1}
input[type=color]::-webkit-color-swatch-wrapper{padding:0}
input[type=color]::-webkit-color-swatch{border:none}
.pal{display:grid;grid-template-columns:repeat(3,1fr);gap:4px}
.pal button{height:24px;padding:0;font-size:7px;letter-spacing:.1em;color:#0a0a0a;
font-weight:700;border:1px solid var(--edge);text-shadow:0 0 2px rgba(255,255,255,.5)}
.nogap .sb>*+*{margin-top:5px}
.nogap .dbody>*+*{margin-top:5px}
.nogap .row>*+*{margin-left:5px}
.nogap .ck>*+*{margin-left:9px}
.nogap .cbar>*+*{margin-left:5px}
.nogap .vrow>*+*{margin-left:8px}
.nogap .logo>*+*{margin-left:7px}
#toast{position:fixed;bottom:76px;left:50%;transform:translateX(-50%) translateY(6px);
background:rgba(9,11,18,.96);border:1px solid var(--sig);color:var(--sig);padding:6px 16px;font-size:10px;
opacity:0;transition:opacity .22s ease,transform .22s ease;pointer-events:none;white-space:nowrap;z-index:210;
max-width:92vw;overflow:hidden;text-overflow:ellipsis;letter-spacing:.12em}
#toast.show{opacity:1;transform:translateX(-50%) translateY(0)}
#toast.err{border-color:var(--blood);color:var(--blood)}
.who{font-size:10px;color:var(--sig2);letter-spacing:.08em;line-height:1.7;
word-break:break-word;min-height:14px}
input[type=text],input[type=password]{flex:1;background:var(--ink);color:var(--bone);border:1px solid var(--edge);
font:16px ui-monospace,Menlo,monospace;padding:9px;letter-spacing:.04em;border-radius:0;min-width:0;
-webkit-appearance:none}
input[type=text]:focus,input[type=password]:focus{outline:none;border-color:var(--sig)}
.frow{display:grid;grid-template-columns:minmax(0,1fr) auto auto auto;gap:6px;align-items:center;
font-size:9px;color:var(--dim);padding:3px 0;border-bottom:1px solid rgba(255,255,255,.05)}
.frow span{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--bone)}
.frow b{color:var(--sig);font-weight:400}
.frow .dl{flex:0 0 auto;padding:4px 10px;font-size:11px;text-decoration:none;
  color:var(--sig);border:1px solid var(--edge);border-radius:3px;
  min-width:34px;text-align:center}
.frow button.dl{font:11px ui-monospace,Menlo,monospace;cursor:pointer}
.sdgrid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:5px;margin-bottom:8px}
.sditem{position:relative;display:block;height:78px;overflow:hidden;background:var(--well);
  border:1px solid var(--edge);color:var(--sig);text-decoration:none}
.sditem img{display:block;width:100%;height:100%;object-fit:cover}
.sditem.video{display:flex;align-items:center;justify-content:center;font-size:22px}
.sditem span{position:absolute;left:0;right:0;bottom:0;padding:2px 4px;background:rgba(0,0,0,.75);
  color:var(--bone);font-size:8px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.strip{display:grid;grid-template-columns:repeat(3,1fr);gap:4px}
.tile{position:relative;cursor:pointer;border:1px solid var(--edge);overflow:hidden;
height:54px;background:#0a0c12}
.tile img{width:100%;height:100%;object-fit:cover;display:block}
.tile span{position:absolute;left:0;right:0;bottom:0;font-size:8px;padding:1px 3px;
background:rgba(0,0,0,.62);color:var(--sig);letter-spacing:.05em;
overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#pov{position:fixed;inset:0;background:rgba(3,4,8,.985);z-index:240;
  display:none;align-items:center;justify-content:center;padding:18px}
#pov.on{display:flex}
.pbrand{font-size:26px;font-weight:700;letter-spacing:.06em;color:var(--onwell);
  display:flex;flex-direction:column;gap:3px}
.pbrand em{font-style:normal;font-size:11px;letter-spacing:.3em;color:var(--onwell2)}
.pinvite{font-size:15px;line-height:1.5;letter-spacing:.04em;color:var(--onwell);
  padding:12px 0 2px}
.psteps{font-size:10px;letter-spacing:.24em;color:var(--onwell2);padding-bottom:6px}
#pjin{width:100%;box-sizing:border-box;padding:11px;border-radius:9px;font:inherit;
  font-size:15px;border:1px solid var(--shadow);background:var(--well);
  color:var(--onwell)}
#pbar{display:none;gap:10px;justify-content:space-between;align-items:baseline;
  padding:7px 13px;font-size:10.5px;letter-spacing:.18em;
  color:var(--onwell2);border-bottom:1px solid var(--shadow)}
body.pasar #pbar{display:flex}
#pbar span:first-child{color:var(--onwell)}
#pflash{position:fixed;left:50%;transform:translate(-50%,-14px);top:0;z-index:239;
  padding:8px 15px;border-radius:0 0 11px 11px;font-size:11px;letter-spacing:.16em;
  background:var(--face2);color:var(--onwell);border:1px solid var(--lip);
  border-top:0;opacity:0;pointer-events:none;transition:opacity .25s,transform .25s;
  white-space:nowrap;max-width:92vw;overflow:hidden;text-overflow:ellipsis}
#pflash.on{opacity:1;transform:translate(-50%,0)}
@media (prefers-reduced-motion:reduce){#pflash{transition:none}}
/* The one control that behaves exactly like the switch on the device:
   tap photographs, a three second hold begins recording, the next press
   stops it. The ring fills during the hold so the three seconds are visible
   rather than guessed at. */
.chk{display:flex;align-items:center;gap:8px;font-size:12px;color:var(--dim);
  padding:6px 0;cursor:pointer}
.chk input{width:16px;height:16px;accent-color:var(--sig)}
.namerow{display:flex;gap:7px;align-items:stretch}
.namerow input{flex:1 1 auto;min-width:0;padding:8px 10px;border-radius:7px;
  font:inherit;font-size:14px;border:1px solid var(--shadow);
  background:var(--well);color:var(--onwell)}
.namerow button{flex:0 0 auto;padding:8px 14px}
.hint{display:block;font-size:10.5px;letter-spacing:.04em;color:var(--dim);
  text-transform:none;margin-top:2px}
#hwbtn{position:relative;width:92px;height:92px;margin:10px auto 4px;
  display:flex;align-items:center;justify-content:center;cursor:pointer;
  user-select:none;-webkit-user-select:none;-webkit-tap-highlight-color:transparent;
  touch-action:none}
#hwbtn svg{position:absolute;inset:0;width:100%;height:100%;transform:rotate(-90deg)}
#hwbtn .ring{fill:var(--i1);stroke:var(--rim);stroke-width:5}
#hwbtn .arc{fill:none;stroke:var(--sig);stroke-width:5;stroke-linecap:round;
  stroke-dasharray:283;stroke-dashoffset:283;transition:stroke-dashoffset .08s linear}
#hwbtn.armed .ring{stroke:var(--sig)}
#hwbtn.recording .ring{stroke:var(--blood);fill:var(--glow1)}
#hwbtn.recording .arc{stroke:var(--blood);stroke-dashoffset:0}
#hwlab{position:relative;font-size:10px;letter-spacing:.14em;color:var(--bone);
  text-align:center;line-height:1.25;pointer-events:none}
#hwbtn.recording #hwlab{color:var(--blood)}
@media (prefers-reduced-motion:reduce){#hwbtn .arc{transition:none}}
#dov{position:fixed;inset:0;background:rgba(3,4,8,.97);z-index:222;
  display:none;align-items:center;justify-content:center;padding:18px}
#dov.on{display:flex}
.dcard{width:100%;max-width:420px;display:flex;flex-direction:column;gap:11px;
  padding:16px;border-radius:14px;
  background:linear-gradient(180deg,var(--face2) 0%,var(--face) 47%,var(--face3) 100%);
  box-shadow:0 1px 0 var(--lip) inset,0 -1px 0 var(--shadow) inset,
             0 12px 26px rgba(0,0,0,.5)}
.dhead{display:flex;align-items:baseline;justify-content:space-between;gap:10px}
.dhead span{font-weight:600;letter-spacing:.04em;color:var(--onwell)}
.dhead em{font-style:normal;font-size:11px;color:var(--onwell2);text-align:right}
.dlab{display:flex;flex-wrap:wrap;gap:6px}
.dchip{padding:5px 10px;border-radius:999px;font-size:12px;cursor:pointer;
  border:1px solid var(--lip);background:var(--face3);color:var(--onwell)}
.dchip:active{background:var(--face2)}
#dtxt{width:100%;box-sizing:border-box;padding:10px;border-radius:9px;
  font:inherit;font-size:14px;line-height:1.5;resize:vertical;
  border:1px solid var(--shadow);background:var(--well);color:var(--onwell)}
#sov{position:fixed;top:0;right:0;bottom:0;left:0;background:rgba(3,4,8,.97);z-index:221;
display:none;flex-direction:column;align-items:center;justify-content:center;gap:12px;padding:16px}
#sov.on{display:flex}
#sim{max-width:100%;max-height:62vh;border:1px solid var(--edge);background:#000}
#sov button{flex:1 1 130px;padding:13px 8px}
#vov{position:fixed;top:0;right:0;bottom:0;left:0;background:rgba(3,4,8,.97);z-index:220;display:none;
flex-direction:column;align-items:center;justify-content:center;gap:12px;padding:16px}
#vov.on{display:flex}
#vpl{max-width:100%;max-height:64vh;background:#000;border:1px solid var(--edge)}
.vhint{color:var(--sig);font-size:10px;text-align:center;letter-spacing:.14em}
#vov button{flex:0 0 auto;width:220px;max-width:80vw;padding:13px 10px}
#vov .vrow{display:flex;gap:8px;flex-wrap:wrap;justify-content:center;width:100%}
#vov .vrow button{flex:1 1 140px;width:auto}

/* ══ MATERIAL SYSTEM ═════════════════════════════════════════════════
   Gloss is three stacked things, not a painted highlight: a body gradient
   with a HARD horizon where the reflection flips, a specular sweep over it,
   and rim light on the physical edges. Depth is a tight contact shadow plus
   a wide ambient one.

   Every stop is a token, so a skin is a variable swap — no new rules, no
   extra bytes per skin beyond the values themselves. */
:root{
  --p1:#2a313a; --p2:#1b2027; --p3:#14181e; --p4:#222932;   /* panel   */
  --b1:#333b45; --b2:#20262d; --b3:#171c22; --b4:#2a323b;   /* button  */
  --h1:#333b45; --h2:#20262d; --h3:#12161b; --h4:#252c35;   /* header  */
  --i1:#0d1014; --i2:#151a20;                               /* recess  */
  --well:#04060a;
  --lit:.10;  --shd:.24;          /* highlight / shade strength */
  --ir1:121,194,164; --ir2:164,147,224; --ir3:221,159,174;  /* oil slick */
  --onwell:#79c2a4; --onwell2:#a493e0;   /* text over the dark well */
  --glow1:#1b2129; --glow2:#17141f;
}
/* ── surface etch ─────────────────────────────────────────────────────
   A pattern cut INTO the panel, sitting under the lighting layer so the
   horizon and bevel still read on top of it. Built from gradients only --
   no image, no external asset, nothing to fetch. Skins that want a plain
   surface inherit the `none` below and pay nothing. */
:root{ --etch:none; }
html[data-skin=hangar]{
  /* fine machined scanlines, the way a 2000s game menu screen-doors */
  --etch:repeating-linear-gradient(0deg,
    rgba(255,255,255,.035) 0 1px, transparent 1px 3px);
}
html[data-skin=chassis]{
  /* hazard hatching, low and wide so it reads as plating not wallpaper */
  --etch:repeating-linear-gradient(135deg,
    rgba(232,163,61,.055) 0 2px, transparent 2px 11px);
}
html[data-skin=sigil]{
  /* angular sigil strokes: two opposed diagonals crossing at a shallow
     angle, so the interference makes a lattice rather than stripes */
  --etch:
    repeating-linear-gradient(22deg,
      rgba(216,31,60,.075) 0 1px, transparent 1px 17px),
    repeating-linear-gradient(-38deg,
      rgba(230,218,212,.035) 0 1px, transparent 1px 23px);
}
html[data-skin=fairy]{
  --etch:
    repeating-linear-gradient(28deg,
      rgba(205,239,255,.045) 0 1px, transparent 1px 29px),
    repeating-linear-gradient(-28deg,
      rgba(255,201,227,.035) 0 1px, transparent 1px 37px);
}
html[data-skin=gunmetal]{
  --onwell:#9d7bff; --onwell2:#5fe3ff;
  --void:#0b1113; --edge:#2c393c;
  --p1:#3d4e52; --p2:#26343a; --p3:#1b2427; --p4:#2f3d40;
  --b1:#4a5c60; --b2:#2f3d40; --b3:#1e282b; --b4:#39494d;
  --h1:#4a5c60; --h2:#2c393c; --h3:#151d1f; --h4:#334346;
  --i1:#080f11; --i2:#111b1e; --well:#060b0d;
  --bone:#cdd7d5; --dim:#7f9092; --rim:#5f767b;
  --sig:#9d7bff; --sig2:#5fe3ff; --blood:#d98a3d;
  --ir1:157,123,255; --ir2:95,227,255; --ir3:217,138,61;
  --glow1:#16262a; --glow2:#1a1430;
}
html[data-skin=obsidian]{
  --onwell:#e8b661; --onwell2:#4ee0ff;
  --void:#04040a; --edge:#1b1b2a;
  --p1:#20202e; --p2:#14141d; --p3:#0e0e16; --p4:#1a1a26;
  --b1:#282838; --b2:#181822; --b3:#101018; --b4:#20202c;
  --h1:#282838; --h2:#16161f; --h3:#0a0a11; --h4:#1e1e2a;
  --i1:#08080e; --i2:#111119; --well:#020206;
  --bone:#d8d2c6; --dim:#6f6a80; --rim:#5a5468;
  --sig:#e8b661; --sig2:#4ee0ff; --blood:#ff3c6e;
  --ir1:232,182,97; --ir2:78,224,255; --ir3:255,60,110;
  --glow1:#171726; --glow2:#0e0e1a;
}
html[data-skin=ember]{
  --onwell:#c8703c; --onwell2:#4de0ff;
  --void:#050508; --edge:#3d2114;
  --p1:#2a1a12; --p2:#18100b; --p3:#100a07; --p4:#221510;
  --b1:#33200f; --b2:#1d1309; --b3:#130c06; --b4:#2a1a10;
  --h1:#33200f; --h2:#1b1109; --h3:#0c0705; --h4:#281810;
  --i1:#080503; --i2:#150d08; --well:#000000;
  --bone:#c9c0b0; --dim:#6b6355; --rim:#8a4a28;
  --sig:#c8703c; --sig2:#4de0ff; --blood:#ff5ec8;
  --ir1:200,112,60; --ir2:77,224,255; --ir3:255,94,200;
  --glow1:#20130c; --glow2:#160c14;
}
html[data-skin=seance]{
  --onwell:#3ef0a0; --onwell2:#b8f0d8;
  --void:#03080a; --edge:#123028;
  --p1:#123c30; --p2:#0a2119; --p3:#061410; --p4:#0f3126;
  --b1:#164a3a; --b2:#0d2a20; --b3:#071a14; --b4:#134034;
  --i1:#02100c; --i2:#0a1f19; --well:#010506;
  --bone:#b8f0d8; --dim:#4d8471; --rim:#2f7a60;
  --h1:#164a3a; --h2:#0b2820; --h3:#04100c; --h4:#123a2e;
  --sig:#3ef0a0; --sig2:#b8f0d8; --blood:#ff6b6b;
  --ir1:62,240,160; --ir2:184,240,216; --ir3:110,220,255;
  --glow1:#08281e; --glow2:#04161a;
}
html[data-skin=hangar]{
  /* 2000s game-menu steel: desaturated blue-green metal, khaki readouts.
     Nostalgic rather than futuristic -- the accent is a worn canvas khaki,
     deliberately not a neon. */
  --onwell:#c9bb92; --onwell2:#a8c4c8;
  --void:#0c1211; --edge:#33403e;
  --p1:#4a5856; --p2:#333e3d; --p3:#232b2a; --p4:#3d4a48;
  --b1:#5a6a67; --b2:#3a4645; --b3:#252e2d; --b4:#46534f;
  --h1:#5a6a67; --h2:#334140; --h3:#1a2120; --h4:#3e4c4a;
  --i1:#0a100f; --i2:#151d1c; --well:#070c0b;
  --bone:#d5d8cd; --dim:#88938d; --rim:#6d7d78;
  --sig:#c9bb92; --sig2:#a8c4c8; --blood:#c4703f;
  --ir1:201,187,146; --ir2:168,196,200; --ir3:196,112,63;
  --glow1:#1c2624; --glow2:#141c1b;
}
html[data-skin=sigil]{
  /* Gothic cyber-sigilism: near-black with a violet cast, arterial crimson,
     bone. The red is a vein, not a glow -- saturated but dark, so it reads
     as something under the surface rather than a light on top of it. */
  --onwell:#e6dad4; --onwell2:#d81f3c;
  --void:#06040a; --edge:#2a1a26;
  --p1:#1c1220; --p2:#120b15; --p3:#0c070e; --p4:#17101a;
  --b1:#241628; --b2:#150d19; --b3:#0e0811; --b4:#1d1322;
  --h1:#241628; --h2:#130c17; --h3:#08050c; --h4:#1a1026;
  --i1:#070410; --i2:#120b18; --well:#040209;
  --bone:#e6dad4; --dim:#7a6470; --rim:#6b4258;
  --sig:#d81f3c; --sig2:#e6dad4; --blood:#ff2e52;
  --ir1:216,31,60; --ir2:230,218,212; --ir3:255,46,82;
  --glow1:#2a0c18; --glow2:#180a22;
}
html[data-skin=chassis]{
  /* Mecha plating: graphite panels, hazard amber, cold ice blue. Industrial
     and legible -- the amber is a warning stripe, used sparingly. */
  --onwell:#e8a33d; --onwell2:#7fc8e8;
  --void:#0a0b0d; --edge:#2b3038;
  --p1:#333944; --p2:#22272e; --p3:#171b21; --p4:#2c323b;
  --b1:#3e4550; --b2:#262c34; --b3:#191d23; --b4:#343b45;
  --h1:#3e4550; --h2:#242a32; --h3:#12161b; --h4:#2e3540;
  --i1:#0b0d11; --i2:#161a20; --well:#07080b;
  --bone:#d7dbe0; --dim:#79818d; --rim:#5d6672;
  --sig:#e8a33d; --sig2:#7fc8e8; --blood:#ff5c3c;
  --ir1:232,163,61; --ir2:127,200,232; --ir3:255,92,60;
  --glow1:#1d232b; --glow2:#241c10;
}
html[data-skin=reliquary]{
  --onwell:#d9c9a4; --onwell2:#b8c9a0;               /* light — for shooting in daylight */
  --void:#ded4bd; --edge:#bfb098;
  --p1:#f6f0e2; --p2:#e7ddc8; --p3:#ddd2ba; --p4:#f0e9d8;
  --b1:#fbf6ea; --b2:#eae0cb; --b3:#ded3bb; --b4:#f4eddc;
  --h1:#fbf6ea; --h2:#e8dec9; --h3:#d3c7ad; --h4:#f2ebda;
  --i1:#cec2a8; --i2:#e2d8c2; --well:#1a1610;
  --bone:#2e2417; --dim:#5f5241; --rim:#9c8b72;
  --sig:#7a2620; --sig2:#3c4f2e; --blood:#8f2119;
  --lit:.72; --shd:.13;
  --ir1:122,38,32; --ir2:60,79,46; --ir3:150,110,62;
  --glow1:#efe7d4; --glow2:#e4dcc8;
}
html[data-skin=fairy]{
  --onwell:#f7f3ff; --onwell2:#cdefff;
  --void:#070710; --edge:#7d7897;
  --p1:#45415a; --p2:#2d2a3d; --p3:#191724; --p4:#39354b;
  --b1:#5f5978; --b2:#39354b; --b3:#211e2e; --b4:#4b4662;
  --h1:#6d6688; --h2:#3b374e; --h3:#15131e; --h4:#504a68;
  --i1:#0b0a13; --i2:#1b1828; --well:#030309;
  --bone:#fbf8ff; --dim:#b7b2c8; --rim:#8e88a8;
  --sig:#cdefff; --sig2:#ffc9e3; --blood:#ff92c7;
  --ir1:205,239,255; --ir2:255,201,227; --ir3:192,177,255;
  --glow1:#252136; --glow2:#19162b;
}

body{
  background:
    radial-gradient(ellipse 110% 70% at 50% -8%, var(--glow1) 0%, transparent 58%),
    radial-gradient(ellipse 80% 50% at 12% 108%, var(--glow2) 0%, transparent 55%),
    var(--void);
}
header{
  background:
    linear-gradient(180deg,rgba(255,255,255,var(--lit)) 0%,rgba(255,255,255,.02) 46%,
                    rgba(0,0,0,var(--shd)) 47%,rgba(0,0,0,.06) 100%),
    linear-gradient(180deg,var(--h1) 0%,var(--h2) 44%,var(--h3) 48%,var(--h4) 100%);
  box-shadow:
    inset 0 2px 0 rgba(255,255,255,calc(var(--lit) * 3.2)),
    inset 0 -2px 0 rgba(0,0,0,.6),
    0 4px 0 rgba(0,0,0,.5),
    0 8px 18px rgba(0,0,0,.45);
  border-bottom:1px solid rgba(0,0,0,.8);
}
.logo{text-shadow:0 1px 2px rgba(0,0,0,.7)}
.logo .mark{color:var(--sig)}

details{
  position:relative;border:none;border-radius:4px;
  background:
    linear-gradient(180deg,rgba(255,255,255,var(--lit)) 0%,rgba(255,255,255,.02) 47%,
                    rgba(0,0,0,var(--shd)) 48%,rgba(0,0,0,.05) 100%),
    var(--etch),
    linear-gradient(180deg,var(--p1) 0%,var(--p2) 47%,var(--p3) 50%,var(--p4) 100%);
  box-shadow:
    inset 0  2px 0 rgba(255,255,255,calc(var(--lit) * 2.4)),   /* top lip   */
    inset 0 -2px 0 rgba(0,0,0,.72),                            /* bottom    */
    inset  2px 0 0 rgba(255,255,255,calc(var(--lit) * .9)),    /* left      */
    inset -2px 0 0 rgba(0,0,0,.52),                            /* right     */
    inset 0 0 0 3px rgba(0,0,0,.14),                           /* machined  */
    0 0 0 1px rgba(0,0,0,.65),                                 /* silhouette*/
    0 3px 5px rgba(0,0,0,.5),
    0 12px 26px rgba(0,0,0,.34);
}
details::after{
  content:'';position:absolute;top:0;right:0;bottom:0;left:0;pointer-events:none;
  border-radius:inherit;
  background:linear-gradient(106deg,transparent 24%,
    rgba(255,255,255,calc(var(--lit) * .95)) 41%,rgba(255,255,255,.02) 52%,transparent 66%);
}
summary{position:relative;z-index:1;color:var(--sig);text-shadow:0 1px 2px rgba(0,0,0,.5)}
.dbody{position:relative;z-index:1}

button{
  position:relative;border:none;border-radius:3px;color:var(--bone);
  text-shadow:0 1px 2px rgba(0,0,0,.5);
  background:
    linear-gradient(180deg,rgba(255,255,255,calc(var(--lit) * 1.15)) 0%,rgba(255,255,255,.02) 47%,
                    rgba(0,0,0,calc(var(--shd) * 1.1)) 48%,rgba(0,0,0,.06) 100%),
    linear-gradient(180deg,var(--b1) 0%,var(--b2) 47%,var(--b3) 50%,var(--b4) 100%);
  box-shadow:
    inset 0  2px 0 rgba(255,255,255,calc(var(--lit) * 3.0)),
    inset 0 -3px 0 rgba(0,0,0,.78),
    inset  2px 0 0 rgba(255,255,255,calc(var(--lit) * 1.1)),
    inset -2px 0 0 rgba(0,0,0,.58),
    0 0 0 1px rgba(0,0,0,.7),
    0 3px 0 rgba(0,0,0,.55),          /* the extruded side wall */
    0 5px 8px rgba(0,0,0,.46),
    0 11px 20px rgba(0,0,0,.3);
  transition:filter .13s ease, transform .06s ease, box-shadow .06s ease;
}
button:hover{filter:brightness(1.18)}
button:active{
  transform:translateY(3px);
  filter:brightness(.88);
  box-shadow:
    inset 0  3px 5px rgba(0,0,0,.75),                          /* sunk in */
    inset 0 -2px 0 rgba(255,255,255,calc(var(--lit) * 1.6)),
    inset  2px 0 4px rgba(0,0,0,.4),
    0 0 0 1px rgba(0,0,0,.7),
    0 1px 2px rgba(0,0,0,.4);
}
button:focus-visible{outline:2px solid var(--sig);outline-offset:2px}
button.on{color:var(--sig)}
button.on::before,.tile.live::before{
  content:'';position:absolute;top:0;right:0;bottom:0;left:0;pointer-events:none;
  border-radius:inherit;mix-blend-mode:screen;opacity:.55;
  background:linear-gradient(122deg,rgba(var(--ir1),.22) 0%,rgba(var(--ir2),.18) 40%,
             rgba(var(--ir3),.14) 74%,rgba(var(--ir1),.18) 100%);
}
button.ron{color:var(--blood);
  box-shadow:inset 0 1px 0 rgba(255,255,255,calc(var(--lit) * 2.2)),
             inset 0 -1px 0 rgba(0,0,0,.6),0 0 11px rgba(0,0,0,.2),
             0 2px 4px rgba(0,0,0,.45);}

select,input[type=text],input[type=color]{
  border:none;border-radius:3px;color:var(--bone);
  background:
    linear-gradient(180deg,rgba(0,0,0,.42) 0%,rgba(0,0,0,.1) 40%,rgba(255,255,255,.05) 100%),
    linear-gradient(180deg,var(--i1) 0%,var(--i2) 100%);
  box-shadow:
    inset 0 3px 6px rgba(0,0,0,.72),
    inset 0 1px 0 rgba(0,0,0,.85),
    inset 0 -2px 0 rgba(255,255,255,calc(var(--lit) * 1.5)),
    inset -1px 0 0 rgba(255,255,255,calc(var(--lit) * .5));
}
select:focus,input[type=text]:focus{outline:none;
  box-shadow:inset 0 2px 4px rgba(0,0,0,.6),0 0 0 1px var(--sig)}
.sl input[type=range]{accent-color:var(--sig)}

.sv{background:var(--well);
  box-shadow:
    inset 0 5px 14px rgba(0,0,0,.95),
    inset 0 2px 0 rgba(0,0,0,.9),
    inset 0 -2px 0 rgba(255,255,255,.13),
    inset 3px 0 8px rgba(0,0,0,.55),
    inset -3px 0 8px rgba(0,0,0,.55);}
.sv::after{
  content:'';position:absolute;top:0;right:0;bottom:0;left:0;pointer-events:none;z-index:9;
  background:linear-gradient(168deg,rgba(255,255,255,.09) 0%,rgba(255,255,255,.02) 26%,
             transparent 46%);
}
.cnr{border-color:var(--rim)}
#fps{color:var(--onwell);text-shadow:0 1px 2px rgba(0,0,0,.85)}
#phase{color:var(--onwell2);text-shadow:0 1px 2px rgba(0,0,0,.85)}
#rb{background:rgba(0,0,0,.45);border-color:var(--blood);color:var(--blood);
    box-shadow:0 0 12px rgba(0,0,0,.4)}

.tile{position:relative;border:none;border-radius:3px;
  background:linear-gradient(160deg,var(--p1),var(--p3));
  box-shadow:
    inset 0 2px 0 rgba(255,255,255,calc(var(--lit) * 2.2)),
    inset 0 -2px 0 rgba(0,0,0,.7),
    0 0 0 1px rgba(0,0,0,.6),
    0 3px 0 rgba(0,0,0,.45), 0 5px 9px rgba(0,0,0,.4);}
.tile:active{transform:translateY(2px);
  box-shadow:inset 0 2px 5px rgba(0,0,0,.7),0 0 0 1px rgba(0,0,0,.6);}
.tile span{background:rgba(0,0,0,.62);color:var(--sig)}
.cbar{background:linear-gradient(180deg,var(--i2),var(--i1));
  box-shadow:inset 0 1px 0 rgba(255,255,255,var(--lit));border-top:1px solid rgba(0,0,0,.5);}
.sb{background:linear-gradient(180deg,var(--i2),var(--void))}
#toast{border-color:var(--sig);color:var(--sig);
  background:linear-gradient(180deg,var(--p1),var(--p3));
  box-shadow:0 4px 14px rgba(0,0,0,.55),inset 0 1px 0 rgba(255,255,255,calc(var(--lit) * 1.6));}
.frow b{color:var(--sig)}
@media(prefers-reduced-motion:reduce){button{transition:none}}

@media(min-width:1500px){ .sb{width:300px} }
@media(min-width:821px) and (max-width:1100px){ .sb{width:210px} }
/* tablet landscape: keep the sidebar, it fits */
@media(max-width:820px) and (orientation:landscape){
  .main{flex-direction:row}
  .sv{height:auto;flex:1;max-height:none}
  .sb{width:200px;border-left:1px solid var(--edge);border-top:none;flex:none}
  .sigil{width:70vh;height:70vh}
}
@media(max-width:820px) and (orientation:portrait){
  .main{flex-direction:column}
  .sv{height:56vw;flex:none;flex-shrink:0;min-height:150px;max-height:50vh}
  .sb{width:100%;border-left:none;border-top:1px solid var(--edge);flex:1;min-height:0}
  .sigil{width:86vw;height:86vw;width:min(52vh,86vw);height:min(52vh,86vw)}
}
/* coarse pointers get bigger targets; fine pointers get hover affordances */
@media(pointer:coarse){ button{padding:11px 5px} .ck input{width:18px;height:18px} }
@media(pointer:fine){ button:hover{border-color:var(--sig);color:var(--sig)} }
@media(prefers-reduced-motion:reduce){
  .logo .mark,#rb.on{animation:none}
}

/* Cybercore instrument shell. The raster is a 13.7 KB flash asset; it is
   decoration around the live image, never an overlay that obscures it. */
:root{--sidebar-w:280px;--skin-art:url('/ui-art/nacre.jpg')}
:root[data-skin="gunmetal"]{--skin-art:url('/ui-art/gunmetal.jpg')}
:root[data-skin="obsidian"]{--skin-art:url('/ui-art/obsidian.jpg')}
:root[data-skin="ember"]{--skin-art:url('/ui-art/ember.jpg')}
:root[data-skin="seance"]{--skin-art:url('/ui-art/seance.jpg')}
:root[data-skin="hangar"]{--skin-art:url('/ui-art/hangar.jpg')}
:root[data-skin="chassis"]{--skin-art:url('/ui-art/chassis.jpg')}
:root[data-skin="sigil"]{--skin-art:url('/ui-art/sigil.jpg')}
:root[data-skin="reliquary"]{--skin-art:url('/ui-art/reliquary.jpg')}
:root[data-skin="fairy"]{--skin-art:url('/ui-art/fairy.jpg')}
.sv{isolation:isolate;background:#030508}
.sv::before{content:'';position:absolute;inset:0;z-index:0;pointer-events:none;
  background:var(--skin-art) center/100% 100% no-repeat;opacity:.72}
#si,#gc{z-index:2;max-width:calc(100% - 54px);max-height:calc(100% - 42px);
  box-shadow:0 0 0 1px rgba(121,194,164,.25),0 0 26px rgba(0,0,0,.9)}
.splitter{width:9px;flex:0 0 9px;cursor:col-resize;touch-action:none;position:relative;
  background:linear-gradient(90deg,#080b0d,#315c5b 45%,#090c0e);z-index:30}
.splitter::after{content:'';position:absolute;left:3px;top:44%;width:3px;height:34px;
  border-top:1px solid var(--sig);border-bottom:1px solid var(--sig);opacity:.7}
.sb{width:clamp(220px,var(--sidebar-w),520px);padding:0 7px 8px;gap:5px;
  background-color:var(--void);background-image:linear-gradient(rgba(6,7,10,.92),rgba(6,7,10,.98)),var(--skin-art);
  background-size:auto,cover;background-attachment:local}
.side-head{position:sticky;top:0;z-index:40;margin:0 -7px 2px;padding:7px;
  border-bottom:1px solid #315c5b;background:rgba(5,8,10,.97)}
.side-ident{height:45px;padding:7px 9px;display:flex;align-items:flex-end;justify-content:space-between;
  background:linear-gradient(90deg,rgba(3,7,8,.58),rgba(3,7,8,.88)),var(--skin-art) center/cover;
  border:1px solid #315c5b;color:var(--bone);font-size:9px;letter-spacing:.18em}
.side-ident b{color:var(--sig);font-size:12px;letter-spacing:.3em}
.side-nav{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:3px;margin-top:5px}
.side-nav button{min-width:0;padding:7px 1px 6px;font-size:8px;letter-spacing:0;
  border-radius:0;box-shadow:inset 0 1px rgba(255,255,255,.08),0 1px #000}
.side-nav button span{display:block;color:var(--sig);font-size:13px;line-height:13px;margin-bottom:2px}
.side-size{display:grid;grid-template-columns:44px 1fr 34px;gap:5px;align-items:center;
  margin-top:5px;color:var(--dim);font-size:8px;letter-spacing:.08em}
.side-size input{width:100%;height:17px;accent-color:var(--sig)}
.side-size output{text-align:right;color:var(--sig)}
.sb details{border-radius:1px;box-shadow:inset 0 1px rgba(255,255,255,.08),0 2px 0 #020304}
.sb summary{min-height:36px;padding:8px 9px}
.sb details[open] summary{border-bottom:1px solid rgba(121,194,164,.18)}
@media(max-width:820px) and (orientation:landscape){
  .sb{width:clamp(200px,var(--sidebar-w),48vw)}
  #si,#gc{max-width:calc(100% - 32px);max-height:calc(100% - 28px)}
}
@media(max-width:820px) and (orientation:portrait){
  .splitter{display:none}.sb{width:100%;padding:0 7px 8px}
  .side-head{position:sticky}.side-size{display:none}
  .side-ident{height:34px;padding:5px 8px}
  #si,#gc{max-width:100%;max-height:100%;box-shadow:none}
  .sv::before{opacity:.32}
  /* Keep the shutter accessible without turning the action bar into a second
     control panel. Parameters retain the useful vertical space. */
  .cbar{flex-wrap:nowrap;align-items:center;gap:4px;padding:4px 6px;overflow-x:auto}
  .cbar>div[style]{display:none}
  .cbar button{flex:0 0 auto;min-width:86px;padding:7px 6px;font-size:9px;white-space:nowrap}
  #hwbtn{width:58px;height:58px;flex:0 0 58px;margin:0 2px}
  #hwbtn .ring,#hwbtn .arc{stroke-width:4}
  #hwlab{font-size:8px;letter-spacing:.1em}
}
</style>
</head>
<body>
<header>
  <span class="logo"><span class="mark">&#9678;</span>WUW&#8202;CAM</span>
  <span class="hstat"><b id="lr">SVGA</b> &nbsp;SD <span id="lsd">--</span>&nbsp; <span id="lp"></span>&nbsp; v1.0</span>
</header>
<div class="main">
  <div class="sv">
    <canvas id="gc"></canvas>
    <img id="si" alt="" decoding="async">
    <div class="cnr tl"></div><div class="cnr tr"></div>
    <div class="cnr bl"></div><div class="cnr br"></div>
    <div id="phase">ATTENDING</div>
    <div id="rb">&#9210; REC <span id="rt">00:00</span></div>
    <div id="fps">--</div>
    <div id="se">&#9678; AWAITING SIGNAL</div>
  </div>
  <div class="splitter" id="splitter" role="separator" aria-label="Resize controls" aria-orientation="vertical"></div>
  <aside class="sb" id="sidebar">
  <div class="side-head">
    <div class="side-ident"><b>WUW-01</b><span>CYBER OPTICAL UNIT</span></div>
    <nav class="side-nav" aria-label="Control groups">
      <button onclick="jumpPanel('g-controls')"><span>&#9678;</span>CORE</button>
      <button onclick="jumpPanel('g-stream')"><span>&#9661;</span>LIVE</button>
      <button onclick="jumpPanel('g-session')"><span>&#10696;</span>ROOM</button>
      <button onclick="jumpPanel('g-fx')"><span>&#10022;</span>FX</button>
      <button onclick="jumpPanel('g-rec')"><span>&#9210;</span>REC</button>
      <button onclick="jumpPanel('g-card')"><span>&#9635;</span>CARD</button>
      <button onclick="jumpPanel('g-network')"><span>&#9788;</span>NET</button>
    </nav>
    <label class="side-size"><span>WIDTH</span><input id="sbsize" type="range" min="220" max="520" value="280" oninput="setSidebarWidth(this.value,true)"><output id="sbout">280</output></label>
  </div>
<details id="g-controls"><summary>△ CONTROLS</summary>
<div class="dbody">
  <div class="lbl">Skin</div>
  <div class="lbl">saved looks
    <span class="hint">kept on the card &mdash; they survive a reflash and travel with it</span></div>
  <select id="sel-preset" onchange="presetLoad(this.value)">
    <option value="">&mdash; saved looks &mdash;</option>
  </select>
  <div class="namerow">
    <input id="presetname" maxlength="24" placeholder="name this look"
           autocomplete="off" autocapitalize="off" spellcheck="false">
    <button onclick="presetSave()">SAVE</button>
    <button onclick="presetDelete()">DEL</button>
  </div>
  <div class="lbl">effect resolution
    <span class="hint">how finely grain, contours and glitch blocks resolve</span></div>
  <select id="sel-fxscale" onchange="setFxScale(this.value)">
    <option value="1">1&times; &mdash; camera resolution</option>
    <option value="1.5">1.5&times; &mdash; finer detail</option>
    <option value="2">2&times; &mdash; sharp (costs frame rate)</option>
    <option value="3">3&times; &mdash; maximum</option>
  </select>
  <div class="lbl">device name
    <span class="hint">what this camera calls itself in the room</span></div>
  <div class="namerow">
    <input id="devname" maxlength="20" placeholder="WUW-01" autocomplete="off"
           autocapitalize="off" spellcheck="false">
    <button onclick="saveDevName()">SET</button>
  </div>
  <select id="sel-skin" onchange="setSkin(this.value)">
    <option value="nacre">Black Nacre &mdash; jade &amp; oil slick</option>
    <option value="gunmetal">Gunmetal &mdash; steel &amp; violet</option>
    <option value="obsidian">Obsidian &mdash; void &amp; gilt</option>
    <option value="ember">Ember &mdash; rust console</option>
    <option value="seance">S&eacute;ance &mdash; phosphor</option>
    <option value="hangar">Hangar &mdash; 2000s steel</option>
    <option value="chassis">Chassis &mdash; mecha plating</option>
    <option value="sigil">Sigil &mdash; gothic, arterial</option>
    <option value="reliquary">Reliquary &mdash; daylight, light</option>
    <option value="fairy">Fairy Angelic &mdash; pearl &amp; moonlight</option>
  </select>
<div class="row">
      <button id="btn-night" onclick="toggleNight()">&#127769; Night</button>
    </div>
    <div class="row">
      <button id="btn-pause" onclick="togglePause()">&#10074;&#10074; Pause</button>
    </div>
</div></details>
<details id="g-stream" open><summary>▽ CAMERA</summary>
<div class="dbody">
<div class="lbl">Live resolution and aspect ratio</div>
    <select id="sel-res" onchange="onRes(this)">
      <optgroup label="4:3">
        <option value="5">QVGA 320x240 · 4:3</option>
        <option value="8">VGA 640x480 · 4:3</option>
        <option value="9" selected>SVGA 800x600 · 4:3</option>
        <option value="10">XGA 1024x768 · 4:3</option>
        <option value="12">SXGA 1280x1024 · 5:4</option>
        <option value="13">UXGA 1600x1200 · 4:3</option>
        <option value="17">QXGA 2048x1536 · 4:3</option>
        <option value="21">QSXGA 2560x1920 · 4:3</option>
      </optgroup>
      <optgroup label="16:9">
        <option value="11">HD 1280x720 · 16:9</option>
        <option value="14">FHD 1920x1080 · 16:9</option>
        <option value="18">QHD 2560x1440 · 16:9</option>
      </optgroup>
    </select>
    <div class="lbl">Still resolution and aspect ratio</div>
    <select id="sel-photores" onchange="onPhotoRes(this)">
      <optgroup label="4:3">
        <option value="8">VGA 640x480 · 4:3</option>
        <option value="9">SVGA 800x600 · 4:3</option>
        <option value="10">XGA 1024x768 · 4:3</option>
        <option value="13">UXGA 1600x1200 · 4:3</option>
        <option value="17">QXGA 2048x1536 · 4:3</option>
        <option value="21" selected>QSXGA 2560x1920 · 4:3 / 5MP</option>
      </optgroup>
      <optgroup label="16:9">
        <option value="11">HD 1280x720 · 16:9</option>
        <option value="14">FHD 1920x1080 · 16:9</option>
        <option value="18">QHD 2560x1440 · 16:9</option>
      </optgroup>
    </select>
    <div class="row">
      <button id="btn-mirror" class="on" onclick="toggleMirror()">MIRROR ON</button>
      <button id="btn-vflip" onclick="toggleVFlip()">FLIP OFF</button>
    </div>
    <div class="row">
      <button id="btn-rot" onclick="cycleViewRot()">VIEW ROTATE 90&deg;</button>
    </div>
    <div class="lbl">turns the live view, saved photos and recordings together &middot; the same setting as the camera screen</div>
    <div class="sl"><span>Brightness</span>
      <input id="sl-bright" type="range" min="-2" max="2" value="0"
        oninput="sv('vbright',this.value)" onchange="api('/set?bright='+this.value)">
      <span id="vbright">0</span>
    </div>
    <div class="sl"><span>Zoom</span>
      <input type="range" id="sl-zoom" min="100" max="400" value="100"
        oninput="setZoom(this.value)">
      <span id="zval">1.0x</span>
    </div>
    <div class="lbl">pinch or scroll the image &middot; double-tap resets</div>
    <div class="sl"><span>Denoise</span>
      <input type="range" min="0" max="8" value="0"
        oninput="sv('vdn',this.value);api('/set?denoise='+this.value)">
      <span id="vdn">0</span>
    </div>
    <div class="lbl">Stream speed</div>
    <select id="sel-rate" onchange="setRate(this.value)">
      <option value="0" selected>Max (lowest latency)</option>
      <option value="66">~15 fps</option>
      <option value="100">~10 fps</option>
      <option value="200">~5 fps</option>
      <option value="500">~2 fps (weak signal)</option>
    </select>
    <div class="sl"><span>Live quality</span>
      <input id="sl-streamq" type="range" min="8" max="45" value="12"
        oninput="sv('vq',this.value)" onchange="api('/set?quality='+this.value)">
      <span id="vq">12</span>
    </div>
    <div class="sl"><span>Still quality</span>
      <input id="sl-photoq" type="range" min="4" max="20" value="4"
        oninput="sv('vpq',this.value)" onchange="api('/set?photoq='+this.value)">
      <span id="vpq">4</span>
    </div>
    <div class="lbl">Lower JPEG quality numbers preserve more detail and use more memory.</div>
</div></details>
<details id="g-session" open><summary>&#9678; SHARED SESSION</summary>
<div class="dbody">
  <div class="lbl">Everyone here shoots the same camera</div>
  <div class="row">
    <input id="nameIn" type="text" maxlength="16" placeholder="your name"
           onchange="setName(this.value)">
  </div>
  <div class="lbl">present (<span id="whoN">1</span>)</div>
  <div id="who" class="who">just you</div>
  <div class="row" style="margin-top:5px">
    <button id="btn-shoot" onclick="fireShot()">&#9673; SHOOT FOR EVERYONE</button>
  </div>
  <div class="lbl" style="margin-top:6px">gallery &mdash; tap a shot to make it yours</div>
  <div id="strip" class="strip"><div class="lbl">no shots yet</div></div>
</div></details>
<details><summary>&#10696; RIG &mdash; OTHER CAMERAS</summary>
<div class="dbody">
  <div class="lbl">Cameras on this network, found by themselves</div>
  <div id="rigwho" class="who">looking&hellip;</div>
  <div class="row" style="margin-top:5px">
    <button id="btn-rigfire" onclick="rigFire()">&#9678; FIRE THE WHOLE RIG</button>
  </div>
  <div class="lbl">every camera shoots the same instant &mdash; one subject, many angles</div>
  <div id="rigpull" class="lbl" style="margin-top:6px">&nbsp;</div>
</div></details>
<details><summary>&#9202; PRE-ROLL</summary>
<div class="dbody">
  <div class="lbl">Hold the last seconds in memory. Save what happened <i>before</i> you pressed.</div>
  <div class="row">
    <button id="btn-prearm" onclick="preArm()">ARM</button>
    <button onclick="preSave()">&#11015; SAVE THE SECONDS</button>
  </div>
  <div id="prestat" class="lbl" style="margin-top:6px">off</div>
  <div class="lbl">fills from the frames the viewfinder is already showing &mdash;
      close every viewer and the ring stops filling</div>
</div></details>
<details open><summary>◉ EXPOSURE ENGINE</summary>
<div class="dbody">
<div class="lbl">Shutter is a duration, not an instant</div>
    <select id="sel-exp">
      <option value="90">Light Painting (lighten)</option>
      <option value="91">Long Exposure (average)</option>
      <option value="92">Persistence (darken)</option>
      <option value="93">Slit Scan (time as space)</option>
    </select>
    <div class="lbl">Slit sweep / reference time</div>
    <select id="sel-expd">
      <option value="6000">6 s</option>
      <option value="12000" selected>12 s</option>
      <option value="30000">30 s</option>
      <option value="60000">60 s</option>
    </select>
    <div class="row" style="margin-top:4px">
      <button id="btn-exp" onclick="toggleExp()">&#9673; EXPOSE</button>
      <button onclick="expReset()">RESET</button>
    </div>
    <div class="row">
      <button id="btn-mt" onclick="toggleMotion()">&#9678; MOTION FIRE</button>
    </div>
    <div class="sl"><span>Sens</span>
      <input type="range" min="4" max="60" value="14"
        oninput="sv('vmt',this.value);mtThresh=parseInt(this.value,10)">
      <span id="vmt">14</span>
    </div>
    <div class="lbl">motion level: <span id="mtv">--</span></div>
</div></details>
<details id="g-fx" open><summary>✦ FX — GPU (TD STYLE)</summary>
<div class="dbody">
<div class="lbl">Effect (live view + MP4/PNG saves)</div>
    <select id="sel-fx" onchange="fxN=parseInt(this.value,10);fxLabels()">
      <option value="0">None</option>
      <optgroup label="3D / Reconstruction">
        <option value="11">Point Cloud 3D</option>
        <option value="10">Relief 3D</option>
        <option value="12">Voxel Extrude</option>
      </optgroup>
      <optgroup label="Feedback / Time">
        <option value="1">Feedback Trails</option>
        <option value="50">Datamosh (motion smear)</option>
        <option value="51">Kuwahara (painterly)</option>
        <option value="52">Flow Streak</option>
        <option value="53">Prism Shatter</option>
        <option value="54">Tunnel (feedback)</option>
        <option value="55">Spectral Bloom</option>
        <option value="56">Flow Paint (structure tensor)</option>
        <option value="57">Caustic Glass</option>
        <option value="58">Relief (parallax depth)</option>
        <option value="59">Rosette (CMYK halftone)</option>
        <option value="60">Chroma Echo</option>
        <option value="16">Slit Scan</option>
        <option value="18">Optical Smear</option>
      </optgroup>
      <optgroup label="Optical">
        <option value="2">Bloom Glow</option>
        <option value="3">RGB Split</option>
        <option value="17">Chroma Fisheye</option>
        <option value="4">Noise Displace</option>
        <option value="13">Kaleidoscope</option>
      </optgroup>
      <optgroup label="Graphic / Print">
        <option value="14">Halftone CMY</option>
        <option value="6">Dither Posterize</option>
        <option value="5">Edge Detect</option>
        <option value="15">Contour Topo</option>
        <option value="9">Pixelate</option>
      </optgroup>
      <optgroup label="Dreamy / Diffusion">
        <option value="20">Orton Dream</option>
        <option value="23">Pro-Mist Soft</option>
        <option value="21">Halation</option>
        <option value="22">Anamorphic Bloom</option>
        <option value="33">Light Leaks</option>
        <option value="24">Tilt-Shift</option>
      </optgroup>
      <optgroup label="Film / Grade">
        <option value="26">Teal &amp; Orange</option>
        <option value="25">Bleach Bypass</option>
        <option value="28">Cross Process</option>
        <option value="27">Day for Night</option>
      </optgroup>
      <optgroup label="VFX Passes">
        <option value="30">Depth Pass (Z)</option>
        <option value="31">Normal Pass</option>
        <option value="32">Atmos Z-Fog</option>
      </optgroup>
      <optgroup label="Surreal / Warp">
        <option value="42">Vortex Twirl</option>
        <option value="43">Liquid Ripple</option>
        <option value="44">Zoom Blur</option>
        <option value="46">Mirror Symmetry</option>
        <option value="48">Bokeh Highlights</option>
      </optgroup>
      <optgroup label="Tone / Duotone">
        <option value="40">Gradient Map</option>
        <option value="41">Solarize</option>
        <option value="47">Toon Cel</option>
      </optgroup>
      <optgroup label="Digital / Broken">
        <option value="45">CRT Scanlines</option>
        <option value="49">Glitch Blocks</option>
      </optgroup>
      <optgroup label="Analog / Colour">
        <option value="7">VHS Analog</option>
        <option value="8">Thermal LUT</option>
      </optgroup>
    </select>
    <div class="sl"><span id="lb-a">Amount</span>
      <input type="range" min="0" max="100" value="50"
        oninput="sv('vga',this.value);fxAmt=this.value/100">
      <span id="vga">50</span>
    </div>
    <div class="sl"><span id="lb-d">Depth</span>
      <input type="range" min="0" max="100" value="50"
        oninput="sv('vgd',this.value);fxDepth=this.value/100">
      <span id="vgd">50</span>
    </div>
    <div class="sl"><span id="lb-s">Speed</span>
      <input type="range" min="0" max="200" value="50"
        oninput="sv('vgs',this.value);fxSpeed=this.value/100">
      <span id="vgs">50</span>
    </div>
    <div class="sl"><span>Wet/Dry</span>
      <input type="range" min="0" max="100" value="100"
        oninput="sv('vgw',this.value);fxWet=this.value/100">
      <span id="vgw">100</span>
    </div>
    <div class="lbl">Apply the look to&hellip;</div>
    <select onchange="setMaskMode(this.value)">
      <option value="0">Whole frame</option>
      <option value="1">Subject only</option>
      <option value="2">Background only</option>
      <option value="3">Subject outline only</option>
    </select>
    <div class="lbl">Subject detected by</div>
    <select onchange="setMaskSrc(this.value)">
      <option value="motion">Motion vs learned scene</option>
      <option value="placeholder">Fixed centre shape</option>
    </select>
    <div class="sl"><span>Sensitivity</span>
      <input type="range" min="2" max="30" value="10"
        oninput="sv('vmk',this.value);maskThresh=this.value/100">
      <span id="vmk">10</span>
    </div>
    <div class="row"><button onclick="relearnBg()">&#8635; RELEARN SCENE</button></div>
    <div class="lbl">Stroke A / Stroke B &mdash; contour, edge, leaks, fog</div>
    <div class="row" id="colrow">
      <input type="color" id="colA" value="#5af2d9" oninput="setCol()">
      <input type="color" id="colB" value="#ffd980" oninput="setCol()">
      <button onclick="swapCol()" style="flex:0 0 30px">&#8646;</button>
    </div>
    <div id="huerow" style="display:none">
      <div class="sl"><span>Hue A</span>
        <input type="range" min="0" max="360" value="168" oninput="hueSet(0,this.value)">
        <span id="vha">168</span></div>
      <div class="sl"><span>Hue B</span>
        <input type="range" min="0" max="360" value="45" oninput="hueSet(1,this.value)">
        <span id="vhb">45</span></div>
    </div>
    <div class="lbl">Palettes</div>
    <div class="pal">
      <button onclick="pal('#5af2d9','#ffd980')" style="background:linear-gradient(90deg,#5af2d9,#ffd980)">AETHER</button>
      <button onclick="pal('#ff2f5e','#ffb03a')" style="background:linear-gradient(90deg,#ff2f5e,#ffb03a)">EMBER</button>
      <button onclick="pal('#b388ff','#4dffa6')" style="background:linear-gradient(90deg,#b388ff,#4dffa6)">SPECTRE</button>
      <button onclick="pal('#ff4ecd','#3df2ff')" style="background:linear-gradient(90deg,#ff4ecd,#3df2ff)">BLOOM</button>
      <button onclick="pal('#e8dcc0','#8a6a3f')" style="background:linear-gradient(90deg,#e8dcc0,#8a6a3f)">VELLUM</button>
      <button onclick="pal('#ffffff','#6b7280')" style="background:linear-gradient(90deg,#fff,#6b7280)">ASH</button>
    </div>
    <div class="sl"><span>Grain</span>
      <input type="range" min="0" max="100" value="0"
        oninput="sv('vgg',this.value);fxGrain=this.value/100">
      <span id="vgg">0</span>
    </div>
</div></details>
<details><summary>○ IMAGE</summary>
<div class="dbody">
<div class="lbl">Effect</div>
    <select onchange="api('/set?effect='+this.value)">
      <option value="0">None</option>
      <option value="1">Negative</option>
      <option value="2">Grayscale</option>
      <option value="3">Reddish</option>
      <option value="4">Greenish</option>
      <option value="5">Bluish</option>
      <option value="6">Sepia</option>
    </select>
    <div class="lbl">White Balance</div>
    <select id="sel-wb" onchange="api('/set?wb='+this.value)">
      <option value="0">Auto</option>
      <option value="1">Sunny</option>
      <option value="2">Cloudy</option>
      <option value="3">Fluorescent</option>
      <option value="4">Incandescent</option>
    </select>
    <div class="sl"><span>Contrast</span>
      <input type="range" id="sl-contrast" min="-2" max="2" value="0"
        oninput="sv('vco',this.value)" onchange="api('/set?contrast='+this.value)">
      <span id="vco">0</span>
    </div>
    <div class="sl"><span>Sat</span>
      <input id="sl-sat" type="range" min="-2" max="2" value="0"
        oninput="sv('vsa',this.value)" onchange="api('/set?sat='+this.value)">
      <span id="vsa">0</span>
    </div>
    <div class="sl"><span>Sharp</span>
      <input type="range" min="-2" max="2" value="0"
        oninput="sv('vsh',this.value)" onchange="api('/set?sharpness='+this.value)">
      <span id="vsh">0</span>
    </div>
</div></details>
<details id="g-rec"><summary>◈ SD REC — GLITCH FX</summary>
<div class="dbody">
<div class="lbl">Record frame rate &mdash; a ceiling, not a target</div>
    <select id="sel-rfps" onchange="recCfg()">
      <option value="30" selected>Best the sensor gives (about 28 fps)</option>
      <option value="25">25 fps</option>
      <option value="24">24 fps (film)</option>
      <option value="20">20 fps</option>
      <option value="15">15 fps</option>
      <option value="10">10 fps</option>
      <option value="5">5 fps (time-lapse feel)</option>
    </select>
    <div class="lbl">The OV5640 delivers about 28 fps at every size, so higher
      settings would not record faster. Frames are timed as captured, so a
      clip always plays at true speed.</div>
    <div class="lbl">Glitch Effect</div>
    <select id="sel-rfx" onchange="recCfg()">
      <option value="0">None</option>
      <option value="1">Ghost / Echo</option>
      <option value="2">Datamosh</option>
      <option value="3">Freeze</option>
      <option value="4">Corrupt</option>
      <option value="5">Strobe</option>
    </select>
    <div class="sl"><span>Intensity</span>
      <input type="range" id="sl-ri" min="0" max="10" value="5"
        oninput="sv('vri',this.value)" onchange="recCfg()">
      <span id="vri">5</span>
    </div>
    <div class="sl"><span>Delay</span>
      <input type="range" id="sl-rd" min="1" max="6" value="3"
        oninput="sv('vrd',this.value)" onchange="recCfg()">
      <span id="vrd">3</span>
    </div>
</div></details>
<details><summary>☀ EXPOSURE</summary>
<div class="dbody">
<div class="row">
      <button id="btn-aec" class="on" onclick="toggleAEC()">AEC AUTO</button>
      <button id="btn-agc" class="on" onclick="toggleAGC()">AGC AUTO</button>
    </div>
    <div class="sl"><span>AE Level</span>
      <input type="range" id="sl-ae" min="-2" max="2" value="1"
        oninput="sv('vael',this.value)" onchange="api('/set?ae_level='+this.value)">
      <span id="vael">1</span>
    </div>
    <div class="sl"><span>Exposure</span>
      <input type="range" min="0" max="1200" value="300"
        oninput="sv('vaev',this.value)" onchange="api('/set?aec_val='+this.value)">
      <span id="vaev">300</span>
    </div>
    <div class="sl"><span>ISO</span>
      <input type="range" min="0" max="30" value="0"
        oninput="sv('vgain',this.value)" onchange="api('/set?agc_gain='+this.value)">
      <span id="vgain">0</span>
    </div>
    <div class="lbl">Gain Ceiling</div>
    <select onchange="api('/set?gainceiling='+this.value)">
      <option value="0">2x</option>
      <option value="1">4x</option>
      <option value="2" selected>8x</option>
      <option value="3">16x</option>
      <option value="4">32x</option>
      <option value="5">64x</option>
      <option value="6">128x</option>
    </select>
    <div class="ck">
      <label><input type="checkbox" checked onchange="api('/set?lenc='+(this.checked?1:0))"> Lens</label>
      <label><input type="checkbox" checked onchange="api('/set?raw_gma='+(this.checked?1:0))"> Gamma</label>
      <label><input type="checkbox" checked onchange="api('/set?dcw='+(this.checked?1:0))"> DCW</label>
    </div>
</div></details>
<details id="g-network"><summary>&#9788; EXTERNAL WIFI</summary>
<div class="dbody">
  <div class="lbl">Join nearby WiFi for firmware updates and to link with other cameras. The wuw access point stays on.</div>
  <div class="row"><button id="btn-scan" onclick="scanNet()">SCAN NEARBY</button></div>
  <select id="wScan" onchange="pickNet(this.value)">
    <option value="">Nearby networks appear here</option>
  </select>
  <div class="row"><input type="text" id="wSsid" placeholder="network name"
    autocomplete="off" autocapitalize="none" spellcheck="false" inputmode="text"></div>
  <div class="row"><input type="password" id="wPass" placeholder="WiFi password"
    autocomplete="current-password" autocapitalize="none" spellcheck="false" enterkeyhint="go"></div>
  <div class="row"><button onclick="saveNet()">CONNECT</button></div>
  <div id="netState" class="lbl">not connected</div>
</div></details>
<details id="g-ap"><summary>&#128274; CAMERA PASSWORD</summary>
<div class="dbody">
  <div class="lbl" id="apState">checking...</div>
  <div class="lbl">The password phones use to join this camera's WiFi. Choose your own so nobody else can watch it or download from its card. 8 to 63 characters.</div>
  <div class="row"><input type="password" id="apCur" placeholder="current password"
    autocomplete="off" autocapitalize="none" spellcheck="false"></div>
  <div class="row"><input type="password" id="apNew" placeholder="new password"
    autocomplete="new-password" autocapitalize="none" spellcheck="false"></div>
  <div class="row"><input type="password" id="apNew2" placeholder="new password again"
    autocomplete="new-password" autocapitalize="none" spellcheck="false" enterkeyhint="go"></div>
  <div class="row"><label><input type="checkbox" onchange="apShow(this.checked)"> show</label></div>
  <div class="row"><button id="btn-ap" onclick="apChange()">SET PASSWORD</button></div>
  <div class="lbl">Changing it restarts the camera's WiFi, so your phone will drop off. Join the camera again with the new password. Forgotten? Hold the shutter button while powering on for 8 seconds to restore the factory password.</div>
</div></details>
<details><summary>&#8681; UPDATES</summary>
<div class="dbody">
  <div class="row" style="margin-top:5px">
    <button onclick="otaCheck()">&#8635; CHECK FOR UPDATE</button>
  </div>
  <div id="otaState" class="lbl">firmware v1.0.0</div>
  <div class="row" id="otaRow" style="display:none">
    <button id="btn-ota" onclick="otaApply()">&#8681; INSTALL UPDATE</button>
  </div>
</div></details>
<details id="g-card" ontoggle="if(this.open)loadFiles()"><summary>▣ SD GALLERY</summary>
<div class="dbody">
  <div class="row">
    <button onclick="loadFiles()">&#8635; REFRESH CARD</button>
    <button onclick="location.href='/archive'">OPEN GALLERY</button>
  </div>
  <div id="files" class="lbl">open this section to read the card</div>
</div></details>
<details><summary>∴ INFO</summary>
<div class="dbody">
<div style="font-size:10px;color:#333;line-height:1.7">
      ESP32-S3 + <span id="cam-model">camera</span><br>
      Photo: sensor maximum /IMG_####.jpg<br>
      Video: /VID_####.mov (MJPEG)<br>
      MJPEG: :81/stream (desktop)<br>
      Visitor connection:<br>
      1. Join WiFi <b>wuw</b><br>
      2. Password <b id="ap-hint">set by the owner</b><br>
      3. Open <b>http://192.168.4.1</b><br>
      Safari and Chrome supported<br>
      keys: space FX still &middot; V video<br>
      S shared shot &middot; E expose<br>
      K cycle skin<br>
      E expose &middot; P pause &middot; [ ] effect
    </div>
</div></details>
</aside>
</div>
<div class="cbar" style="flex-wrap:wrap">
  <button id="btn-cap" onclick="doCapture()">&#128247; SD PHOTO</button>
  <button id="btn-rec" onclick="toggleRec()">&#9210; SD REC</button>
  <div style="flex-basis:100%;height:2px"></div>
  <button onclick="saveJpg()">&#11015; MAX RAW</button>
  <button onclick="fxPhoto()">&#10022; FX STILL</button>
  <button onclick="savePng()">&#11015; PNG</button>
  <button id="btn-drec" onclick="toggleDevRec()">&#9210; MP4</button>
  <button id="btn-shoot2" onclick="fireShot()">&#9673; SHOOT</button>
  <div id="hwbtn" title="tap = photo · hold 3s = record · press again = stop">
    <svg viewBox="0 0 100 100" aria-hidden="true">
      <circle class="ring" cx="50" cy="50" r="45"></circle>
      <circle class="arc"  cx="50" cy="50" r="45"></circle>
    </svg>
    <span id="hwlab">SHUTTER</span>
  </div>
</div>
<div id="sov">
  <img id="sim" alt="">
  <div class="vhint" id="shint"></div>
  <div class="vrow">
    <button onclick="shotMyFx()">&#10022; RENDER WITH MY FX</button>
    <button onclick="shotBwoWall()">&#9638; BWO WALL</button>
    <button onclick="serverDownload()">&#11015; SAVE TO GALLERY</button>
    <button onclick="closeSov()">CLOSE</button>
  </div>
</div>
<div id="pov">
  <div class="dcard">
    <div class="pbrand">WUW-01<em>PASAR PROTOCOL</em></div>
    <div class="pinvite">A CAMERA YOU DON'T HOLD.<br>A CAMERA YOU JOIN.</div>
    <div class="psteps">JOIN &rarr; WATCH &rarr; ALTER &rarr; SHOOT &rarr; WRITE</div>
    <input id="pjin" maxlength="18" placeholder="a name, if you want one"
           autocomplete="off" autocapitalize="off" spellcheck="false">
    <div class="vrow">
      <button onclick="pasarJoin()">ENTER</button>
      <button onclick="pasarSkip()">SKIP</button>
    </div>
  </div>
</div>
<div id="pbar"><span id="pcount">INSIDE THE CAMERA</span><span id="pframe"></span></div>
<div id="pflash"></div>
<div id="dov">
  <div class="dcard">
    <div class="dhead"><span id="dttl">IMG</span><em id="dsrc"></em></div>
    <div id="dlab" class="dlab"></div>
    <textarea id="dtxt" rows="5" placeholder="What happened here?"></textarea>
    <div class="vrow">
      <button onclick="diarySave()">&#9998; SAVE NOTE</button>
      <button onclick="diaryClose()">CLOSE</button>
    </div>
  </div>
</div>
<div id="vov">
  <video id="vpl" controls playsinline></video>
  <div class="vhint" id="vhint">Tap <b>SAVE TO GALLERY</b>&nbsp; &middot; &nbsp;then Share &rarr; Save Video</div>
  <div class="vrow">
    <button onclick="saveOverlayVideo()">&#11015; SAVE TO GALLERY</button>
    <button onclick="closeVov()">CLOSE</button>
  </div>
</div>
<div id="toast"></div>
<script>
/* Skin is applied before anything else so the page never flashes the default
   one first. Each skin is only a set of custom-property values — the material
   rules themselves are shared. */
var SKIN='nacre';
try{ SKIN=localStorage.getItem('wuwskin')||'nacre'; }catch(e){}
document.documentElement.setAttribute('data-skin',SKIN);
function setSkin(k){
  SKIN=k;
  document.documentElement.setAttribute('data-skin',k);
  try{localStorage.setItem('wuwskin',k)}catch(e){}
  fetch('/panel/skin?name='+encodeURIComponent(k),{cache:'no-store'}).catch(function(){});
  if(window.toast)toast('Skin: '+k);
}
function setSidebarWidth(v,remember){
  var max=Math.min(520,Math.max(220,window.innerWidth*.55));
  var n=Math.max(220,Math.min(max,parseInt(v,10)||280));
  document.documentElement.style.setProperty('--sidebar-w',n+'px');
  var r=document.getElementById('sbsize'),o=document.getElementById('sbout');
  if(r)r.value=Math.round(n);if(o)o.value=Math.round(n);
  if(remember)try{localStorage.setItem('wuw-sbw',String(Math.round(n)))}catch(e){}
}
function jumpPanel(id){
  var box=document.getElementById(id),sb=document.getElementById('sidebar');
  if(!box||!sb)return;
  var all=sb.querySelectorAll('details');
  for(var i=0;i<all.length;i++)if(all[i]!==box)all[i].open=false;
  box.open=true;
  setTimeout(function(){box.scrollIntoView({block:'start',behavior:'smooth'})},0);
}
(function initSidebar(){
  var saved=280;try{saved=parseInt(localStorage.getItem('wuw-sbw'),10)||280}catch(e){}
  setSidebarWidth(saved,false);
  var grip=document.getElementById('splitter'),drag=false;
  if(!grip)return;
  grip.addEventListener('pointerdown',function(e){drag=true;grip.setPointerCapture(e.pointerId);e.preventDefault()});
  grip.addEventListener('pointermove',function(e){if(drag)setSidebarWidth(window.innerWidth-e.clientX,true)});
  grip.addEventListener('pointerup',function(){drag=false});
  grip.addEventListener('pointercancel',function(){drag=false});
})();
var img=document.getElementById('si'),se=document.getElementById('se');
var run=true,busy=false,gap=0,tmr=null,wd=null,nf=0,t0=Date.now();
var latAcc=0,latN=0,latShown=0,reqT=0;   // measured request->drawn latency

/* ── GPU FX — TouchDesigner-style TOPs as a WebGL fragment shader ──
   Each streamed frame uploads as a texture; a ping-pong framebuffer pair
   provides the previous OUTPUT frame (real Feedback-TOP behaviour).
   Runs on the viewing device GPU; zero load on the camera board.
   Falls back to the plain <img> stream if WebGL is unavailable. */
function rlog(m){try{fetch('/log?m='+encodeURIComponent(String(m).slice(0,180)))}catch(e){}}
window.onerror=function(m,src,ln){rlog('JSERR '+m+' @'+ln);};
var gc=document.getElementById('gc');
var glWatchOnce=false;      // attached below, once glWatch() exists
var gl=null,glP=null,glU={},glTex=null,fbo=[null,null],fbt=[null,null],pp=0;
var quadBuf=null,pcP=null,pcU={},pcBuf=null,pcN=0,texDirty=false;
var fxN=0,fxAmt=.5,fxDepth=.5,fxSpeed=.5,fxGrain=0,glT0=Date.now();   // grain 0: the default picture is untouched
var glReady=false,texCount=0,gotFirst=false,glFatal=false;
var fxCA=[.35,.95,.85],fxCB=[1,.85,.5],fxWet=1;   // stroke A / stroke B / wet-dry
var fxZoom=1, fxPanX=0, fxPanY=0;                 // digital zoom + pan
/* Subject mask. Whatever produces it — a segmentation model on the phone, or
   the placeholder shape below — it arrives here as one greyscale texture, and
   every one of the 42 looks becomes subject-aware without further changes. */
var maskTex=null, maskMode=0, maskCv=null, maskCx=null, maskSrc='off';
/* Background subtraction: the camera learns the still scene, then anything
   that differs from it is the subject. No model, no downloads, and for a
   camera left on a table it is genuinely accurate. Learned on the GPU, so
   it costs two extra passes rather than any CPU time. */
var bgFbo=[null,null], bgTex=[null,null], bgPP=0, mFbo=null, mTex=null;
var bgRate=0.02, maskThresh=0.10, bgReady=0;
/* 1 = the camera's own resolution. Above that the effects supersample: the
   photograph is upscaled once with linear filtering, then every generated
   pattern is computed on the finer grid. Costs fill rate, not bandwidth --
   the camera still sends exactly the same frames. */
var fxScale=1;
function setFxScale(v){
  fxScale=parseFloat(v)||1;
  try{ localStorage.setItem('wuwfxscale',String(fxScale)); }catch(e){}
  if(gl&&img&&img.naturalWidth) glSize(
    Math.min(2048,Math.round(img.naturalWidth*fxScale)),
    Math.round(img.naturalHeight*fxScale*Math.min(1,2048/(img.naturalWidth*fxScale))));
  toast('Effect resolution '+fxScale+'x');
}
try{ fxScale=parseFloat(localStorage.getItem('wuwfxscale'))||1; }catch(e){}
/* ── tracked object boxes ────────────────────────────────────────────────
   Bounding boxes do not need a neural network: the segmentation mask already
   says which pixels are a moving thing, so the blobs' extents ARE the boxes.
   The mask is downsampled to 40x30 for readback (readPixels stalls the GPU,
   and 4800 bytes is cheap), labelled with a flood fill, then matched to the
   previous frame by overlap so each object keeps its identity while it moves.
   What this does NOT do is say WHAT the object is -- that needs a model. */
var boxes=[], boxMode=0, boxTrk=[], boxNextId=1, boxFrame=0;
var bFbo=null, bTex=null, BXW=40, BXH=30, bPix=null;
var BOX_MIN=6;          // ignore blobs smaller than this many cells
function boxAlloc(){
  if(bTex) gl.deleteTexture(bTex);
  if(bFbo) gl.deleteFramebuffer(bFbo);
  bTex=mkTex(BXW,BXH);
  bFbo=gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER,bFbo);
  gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,bTex,0);
  gl.bindFramebuffer(gl.FRAMEBUFFER,null);
  bPix=new Uint8Array(BXW*BXH*4);
}
function boxDetect(){
  if(!mTex) return;
  if(!bFbo) boxAlloc();
  var savedMask=maskTex; maskTex=null;          // never gate the helper pass
  glPass(bFbo,99,mTex,mTex,BXW,BXH);
  maskTex=savedMask;
  gl.bindFramebuffer(gl.FRAMEBUFFER,bFbo);
  gl.readPixels(0,0,BXW,BXH,gl.RGBA,gl.UNSIGNED_BYTE,bPix);
  gl.bindFramebuffer(gl.FRAMEBUFFER,null);

  var lab=new Int16Array(BXW*BXH), found=[], stack=[];
  for(var i=0;i<BXW*BXH;i++) lab[i]=(bPix[i*4]>120)?-1:0;
  for(var y=0;y<BXH;y++) for(var x=0;x<BXW;x++){
    var p=y*BXW+x;
    if(lab[p]!==-1) continue;
    var id=found.length+1, n=0;
    var x0=x,x1=x,y0=y,y1=y;
    stack.length=0; stack.push(p); lab[p]=id;
    while(stack.length){
      var q=stack.pop(), qx=q%BXW, qy=(q-qx)/BXW; n++;
      if(qx<x0)x0=qx; if(qx>x1)x1=qx; if(qy<y0)y0=qy; if(qy>y1)y1=qy;
      for(var dy=-1;dy<=1;dy++) for(var dx=-1;dx<=1;dx++){
        var nx=qx+dx, ny=qy+dy;
        if(nx<0||ny<0||nx>=BXW||ny>=BXH) continue;
        var np=ny*BXW+nx;
        if(lab[np]===-1){ lab[np]=id; stack.push(np); }
      }
    }
    if(n>=BOX_MIN) found.push({x0:x0/BXW,y0:y0/BXH,x1:(x1+1)/BXW,y1:(y1+1)/BXH,n:n});
  }
  found.sort(function(a,b){return b.n-a.n});
  if(found.length>8) found.length=8;

  // match to the previous frame by overlap, so IDs survive movement
  boxFrame++;
  var used=[];
  for(var f=0;f<found.length;f++){
    var best=-1,bestS=0.15;                     // require real overlap
    for(var t=0;t<boxTrk.length;t++){
      if(used.indexOf(t)>=0) continue;
      var A=found[f],B=boxTrk[t];
      var ix=Math.max(0,Math.min(A.x1,B.x1)-Math.max(A.x0,B.x0));
      var iy=Math.max(0,Math.min(A.y1,B.y1)-Math.max(A.y0,B.y0));
      var inter=ix*iy;
      var uni=(A.x1-A.x0)*(A.y1-A.y0)+(B.x1-B.x0)*(B.y1-B.y0)-inter;
      var s=uni>0?inter/uni:0;
      if(s>bestS){bestS=s;best=t;}
    }
    if(best>=0){
      var T=boxTrk[best]; used.push(best);
      var k=0.45;                               // smooth, or the box jitters
      T.x0+=(found[f].x0-T.x0)*k; T.y0+=(found[f].y0-T.y0)*k;
      T.x1+=(found[f].x1-T.x1)*k; T.y1+=(found[f].y1-T.y1)*k;
      T.seen=boxFrame; found[f].id=T.id;
    } else {
      var nt={id:boxNextId++,x0:found[f].x0,y0:found[f].y0,
              x1:found[f].x1,y1:found[f].y1,seen:boxFrame};
      boxTrk.push(nt); used.push(boxTrk.length-1); found[f].id=nt.id;
    }
  }
  boxTrk=boxTrk.filter(function(T){return boxFrame-T.seen<8});  // forget stale
  boxes=boxTrk.slice(0,8);
}

/* ── Scene labels ────────────────────────────────────────────────────────
   These are HEURISTICS computed from the actual pixels, not a classifier.
   Everything here is measured: subject count comes from the tracker, the
   rest from a 32x24 downsample of the live frame. They are honest scene
   descriptors -- "two figures, warm, low light" -- and they are deliberately
   phrased as description rather than identification, because nothing here
   knows what an object IS.

   classifyWithModel() is the slot where a real model plugs in. It returns
   null until model weights exist on the card; when they do, its labels are
   merged in front of the heuristic ones. */
var SW=32, SH=24, sFbo=null, sTex=null, sPix=null;
var mdlReady=false, mdlName='';
function sceneAlloc(){
  if(sTex) gl.deleteTexture(sTex);
  if(sFbo) gl.deleteFramebuffer(sFbo);
  sTex=mkTex(SW,SH);
  sFbo=gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER,sFbo);
  gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,sTex,0);
  gl.bindFramebuffer(gl.FRAMEBUFFER,null);
  sPix=new Uint8Array(SW*SH*4);
}
function hueName(r,g,b){
  var mx=Math.max(r,g,b), mn=Math.min(r,g,b), d=mx-mn;
  if(mx<40) return 'near black';
  if(d<18) return mx>170?'pale':'grey';
  var h;
  if(mx===r) h=((g-b)/d+6)%6; else if(mx===g) h=(b-r)/d+2; else h=(r-g)/d+4;
  h*=60;
  if(h<15||h>=345) return 'red';
  if(h<45) return 'amber'; if(h<70) return 'yellow'; if(h<160) return 'green';
  if(h<200) return 'teal';  if(h<250) return 'blue';  if(h<290) return 'violet';
  return 'magenta';
}
function classifyWithModel(){ return null; }   // no weights on the card yet
function sceneLabels(){
  if(!gl||!glTex) return {labels:[],stats:null};
  if(!sFbo) sceneAlloc();
  var savedMask=maskTex, savedBox=boxMode;
  maskTex=null; boxMode=0;                       // analyse the plain frame
  glPass(sFbo,99,glTex,glTex,SW,SH);
  maskTex=savedMask; boxMode=savedBox;
  gl.bindFramebuffer(gl.FRAMEBUFFER,sFbo);
  gl.readPixels(0,0,SW,SH,gl.RGBA,gl.UNSIGNED_BYTE,sPix);
  gl.bindFramebuffer(gl.FRAMEBUFFER,null);

  var n=SW*SH, sr=0,sg=0,sb=0, lum=0, sat=0;
  /* readPixels row 0 is the framebuffer's BOTTOM, and the frame was uploaded
     with UNPACK_FLIP_Y so v=0 is the image bottom -- therefore the image's
     TOP is the LAST rows here. Sky lives in the top third. */
  var skyN=0, skyHit=0, grnHit=0;
  for(var y=0;y<SH;y++) for(var x=0;x<SW;x++){
    var i=(y*SW+x)*4, r=sPix[i], g=sPix[i+1], b=sPix[i+2];
    sr+=r; sg+=g; sb+=b;
    var mx=Math.max(r,g,b), mn=Math.min(r,g,b);
    lum+=(r*77+g*150+b*29)>>8;
    sat+=mx?((mx-mn)*255/mx):0;
    if(y>=SH-Math.floor(SH/3)){ skyN++; if(b>r+18 && b>90 && (mx-mn)<150) skyHit++; }
    if(g>r+14 && g>b+14) grnHit++;
  }
  sr/=n; sg/=n; sb/=n; lum/=n; sat/=n;
  var skyFrac=skyN?skyHit/skyN:0, grnFrac=grnHit/n;
  var subj=boxes.length;
  var cov=0; for(var q=0;q<boxes.length;q++)
    cov+=(boxes[q].x1-boxes[q].x0)*(boxes[q].y1-boxes[q].y0);

  var L=[];
  if(boxMode>0 && maskMode>0){          // only if we were actually looking
    if(subj===0)      L.push('empty frame');
    else if(subj===1) L.push(cov>0.22?'close subject':'one figure');
    else if(subj===2) L.push('two figures');
    else              L.push(subj+' figures');
  }
  if(lum<38)       L.push('dark');
  else if(lum<80)  L.push('low light');
  else if(lum>178) L.push('bright');
  else             L.push('soft light');
  if(sr>sb+22)      L.push('warm');
  else if(sb>sr+22) L.push('cool');
  if(skyFrac>0.42)      L.push('sky');
  else if(grnFrac>0.22) L.push('greenery');
  else if(sat<95 && lum<120) L.push('indoors');
  L.push(hueName(sr,sg,sb));
  var m=classifyWithModel();
  if(m && m.length) L=m.concat(L);
  return {labels:L, fromModel:!!m, stats:{lum:Math.round(lum),sat:Math.round(sat),
          sky:+skyFrac.toFixed(2), green:+grnFrac.toFixed(2), subjects:subj,
          rgb:[Math.round(sr),Math.round(sg),Math.round(sb)]}};
}
/* ── the diary ─────────────────────────────────────────────────────────── */
var noteFor='';
function diaryOpen(file){
  noteFor=file||('IMG_'+(window.photoN||0));
  var sl=sceneLabels();
  window._lastLabels=sl;                 // recorded verbatim in the metadata
  var box=document.getElementById('dlab');
  box.innerHTML='';
  sl.labels.forEach(function(t){
    var b=document.createElement('button');
    b.className='dchip'; b.textContent=t;
    b.onclick=function(){ var ta=document.getElementById('dtxt');
      ta.value=(ta.value?ta.value.replace(/\s+$/,'')+' ':'')+t+' '; ta.focus(); };
    box.appendChild(b);
  });
  document.getElementById('dsrc').textContent =
    sl.fromModel ? 'labels from the model on the card'
                 : 'labels from scene analysis - no model on the card';
  document.getElementById('dttl').textContent = noteFor;
  fetch('/note?f='+encodeURIComponent(noteFor)).then(function(r){return r.json()})
    .then(function(d){ if(d&&d.text) document.getElementById('dtxt').value=d.text; })
    .catch(function(){});
  document.getElementById('dov').classList.add('on');
}
/* Composes the two representations the camera will persist. The plain text
   is authoritative and stays readable on any computer; the JSON is an
   addition. Only fields this system can actually know are emitted -- where
   something is unknown it is null, and named object recognition is reported
   as not implemented rather than left ambiguous. */
function pasarCompose(note){
  var sl=(window._lastLabels||{labels:[],fromModel:false});
  var obs=sl.labels.join(' / ');
  var room=(pasarRoom&&pasarRoom.visitors)||1;
  var idn=(noteFor.match(/(\d+)/)||[,'0'])[1];
  var txt=
    'WUW-01 / PASAR PROTOCOL\n'+
    'IMAGE: '+idn+'\n'+
    'SESSION: '+((pasarRoom&&pasarRoom.session)||'-')+'\n'+
    'AUTHOR: '+(pasarMe||'anonymous')+'\n'+
    'ROOM: '+room+' connected\n'+
    (obs?('OBSERVATION: '+obs+'\n'):'')+
    '\nNOTE:\n'+(note||'')+'\n';
  var meta={
    id:parseInt(idn,10),
    session:(pasarRoom&&pasarRoom.session)||null,
    author:pasarMe||null,
    connectedVisitors:room,
    effect:(function(){                 /* the label the user actually chose */
      var o=document.querySelector('#sel-fx option[value="'+fxN+'"]');
      return o?o.textContent.trim():String(fxN);
    })(),
    effectParameters:{amount:+fxAmt.toFixed(3),depth:+fxDepth.toFixed(3),
                      speed:+fxSpeed.toFixed(3),grain:+fxGrain.toFixed(3),
                      wet:+fxWet.toFixed(3),zoom:+fxZoom.toFixed(3)},
    subjectCount:(boxMode>0&&maskMode>0)?boxes.length:null,
    scene:sl.stats?{light:sl.stats.lum<80?'low':(sl.stats.lum>178?'bright':'medium'),
                    temperature:(sl.stats.rgb[0]>sl.stats.rgb[2]+22)?'warm':
                                ((sl.stats.rgb[2]>sl.stats.rgb[0]+22)?'cool':'neutral'),
                    location:sl.labels.indexOf('sky')>=0?'outdoors':
                             (sl.labels.indexOf('indoors')>=0?'indoors':null)}:null,
    observationSource:sl.fromModel?'model':'heuristic',
    objectsNamed:null,          /* named recognition is not implemented */
    timeSource:'device has no clock; see sessionUptimeMs',
    sessionUptimeMs:(pasarRoom&&pasarRoom.sessionUptimeMs)||null,
    note:note||''
  };
  return txt+'\n===WUW-JSON===\n'+JSON.stringify(meta,null,1);
}
function diarySave(){
  var t=document.getElementById('dtxt').value||'';
  var payload = pasarOn ? pasarCompose(t) : t;
  fetch('/note?f='+encodeURIComponent(noteFor),{method:'POST',body:payload})
    .then(function(r){return r.json()})
    .then(function(d){ toast(d&&d.ok?('Saved to '+noteFor+'.txt'):'Save failed',!(d&&d.ok));
                       if(d&&d.ok) diaryClose(); })
    .catch(function(){ toast('Save failed',true); });
}
function diaryClose(){ document.getElementById('dov').classList.remove('on'); }


/* ── PASAR: visitor identity and shared presence ─────────────────────────
   Identity is local only: an alias in this browser's own storage, no account,
   no password, nothing leaves the device. The camera keeps it for 45 seconds
   after it last heard from you, which is what makes the room count real
   rather than cumulative.

   The room is POLLED, not pushed. The camera allows 7 open sockets; a held-
   open stream per visitor would exhaust that at the seventh person. Asking
   for room state is also how you announce you are still here, so presence
   costs no extra request. */
var pasarOn=false, pasarMe='', pasarRoom=null, pasarSeenImage=0, pasarTimer=null;
function pasarAlias(){
  try{ return localStorage.getItem('wuw.alias')||''; }catch(e){ return ''; }
}
function pasarSetAlias(a){
  try{ localStorage.setItem('wuw.alias',a||''); }catch(e){}
  pasarMe=a||'';
}
function pasarPeople(n){
  return n+' '+(n===1?'PERSON':'PEOPLE')+' INSIDE THE CAMERA';
}
function pasarPoll(){
  fetch('/room?as='+encodeURIComponent(pasarMe||''),{cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(d){
      if(!d||!d.exhibition) return;
      pasarOn=true;
      if(d.you && d.you!==pasarMe) pasarSetAlias(d.you);
      pasarRoom=d;
      var c=document.getElementById('pcount');
      if(c) c.textContent=pasarPeople(d.visitors);
      var f=document.getElementById('pframe');
      if(f) f.textContent='IMAGE '+String(d.lastImage).padStart(4,'0')+
                          '  ·  '+d.photos+' THIS SESSION';
      /* someone else released the shutter: everyone in the room feels it */
      if(d.lastImage && d.lastImage!==pasarSeenImage){
        if(pasarSeenImage) pasarFlash(d.lastAuthor,d.lastImage);
        pasarSeenImage=d.lastImage;
      }
    })
    .catch(function(){});
}
function pasarFlash(who,img){
  var el=document.getElementById('pflash');
  if(!el) return;
  el.textContent=(who||'someone').toUpperCase()+' MADE IMAGE '+
                 String(img).padStart(4,'0');
  el.classList.add('on');
  clearTimeout(el._t);
  el._t=setTimeout(function(){ el.classList.remove('on'); },2600);
}
function pasarJoin(){
  var v=document.getElementById('pjin').value||'';
  pasarSetAlias(v.replace(/[^A-Za-z0-9 _-]/g,'').slice(0,18));
  document.getElementById('pov').classList.remove('on');
  pasarPoll();
}
function pasarSkip(){
  document.getElementById('pov').classList.remove('on');
  pasarPoll();
}
function pasarBoot(){
  fetch('/room',{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(d){
      if(!d||!d.exhibition) return;            // standard firmware: do nothing
      pasarOn=true;
      document.body.classList.add('pasar');
      pasarMe=pasarAlias();
      if(!pasarMe) document.getElementById('pov').classList.add('on');
      else pasarPoll();
      if(pasarTimer) clearInterval(pasarTimer);
      pasarTimer=setInterval(pasarPoll,2500);
    })
    .catch(function(){});
}


/* Uploads this visitor's own rendering of the frame just captured. Runs only
   after the camera reports the ORIGINAL was written, and every failure path
   is silent: the archival image is already safe, and an exhibition should
   never show an error for an optional extra. Long edge is capped at 1280 and
   quality at 0.8 -- large enough to project, small enough that twenty phones
   doing this cannot flood the radio or the card. */
function pasarSendView(){
  try{
    if(!gc||!gc.width) return;
    var n=(pasarRoom&&pasarRoom.lastImage!=null)?pasarRoom.lastImage+1:null;
    var MAX=1280, w=gc.width, h=gc.height;
    var sc=Math.min(1,MAX/Math.max(w,h));
    var up=rotated(gc,viewTurn());                    // upright, like what the viewer saw
    w=up.width; h=up.height; sc=Math.min(1,MAX/Math.max(w,h));
    var o=document.createElement('canvas');
    o.width=Math.max(1,Math.round(w*sc)); o.height=Math.max(1,Math.round(h*sc));
    o.getContext('2d').drawImage(up,0,0,o.width,o.height);
    o.toBlob(function(b){
      if(!b) return;
      /* ask the camera which number it actually assigned, rather than guessing */
      fetch('/room?as='+encodeURIComponent(pasarMe||''),{cache:'no-store'})
        .then(function(r){return r.json()})
        .then(function(d){
          if(!d||!d.lastImage) return;
          fetch('/view?n='+d.lastImage,{method:'POST',body:b}).catch(function(){});
        }).catch(function(){});
    },'image/jpeg',0.8);
  }catch(e){}
}


/* ── the web shutter ─────────────────────────────────────────────────────
   Deliberately the SAME state machine as btnTask() in the firmware, including
   the part that catches people out: a hold starts recording while the finger
   is still down, so the release that follows would otherwise read as the stop
   press and kill the clip instantly. Hence the latch. */
var HW_HOLD=3000;
var hwDown=false, hwT0=0, hwTimer=null, hwRaf=null,
    hwHoldFired=false, hwNeedRelease=false, hwRec=false, hwBusy=false;
function hwEl(){ return document.getElementById('hwbtn'); }
function hwLabel(t){ var e=document.getElementById('hwlab'); if(e) e.textContent=t; }
function hwArc(f){
  var a=document.querySelector('#hwbtn .arc');
  if(a) a.style.strokeDashoffset=String(283*(1-Math.max(0,Math.min(1,f))));
}
function hwTick(){
  if(!hwDown||hwHoldFired||hwNeedRelease||hwRec){ hwRaf=null; return; }
  var f=(Date.now()-hwT0)/HW_HOLD;
  hwArc(f);
  if(f>=1){ hwStartRec(); hwRaf=null; return; }
  hwRaf=requestAnimationFrame(hwTick);
}
function hwPress(e){
  if(e&&e.preventDefault) e.preventDefault();
  if(hwBusy||hwDown) return;
  hwDown=true; hwT0=Date.now(); hwHoldFired=false;
  hwEl().classList.add('armed');
  if(hwRec){                                  /* pressing while recording stops */
    hwNeedRelease=true; hwStopRec(); return;
  }
  hwRaf=requestAnimationFrame(hwTick);
}
function hwRelease(e){
  if(e&&e.preventDefault) e.preventDefault();
  if(!hwDown) return;
  hwDown=false;
  hwEl().classList.remove('armed');
  if(hwRaf){ cancelAnimationFrame(hwRaf); hwRaf=null; }
  hwArc(0);
  if(hwNeedRelease){ hwNeedRelease=false; return; }   /* the latch */
  if(!hwHoldFired && !hwRec) hwShoot();
  hwHoldFired=false;
}
function hwShoot(){
  if(hwBusy) return; hwBusy=true; hwLabel('...');
  api('/capture'+(pasarOn?('?as='+encodeURIComponent(pasarMe)):''))
    .then(function(d){
      hwBusy=false;
      var r=String((d&&d.result)||'');
      if(r==='BUSY'){ hwLabel('WAIT'); toast('Shutter busy — try again',true); }
      else if(r.indexOf('ERR')>=0){ hwLabel('SHUTTER'); toast(r,true); }
      else { hwLabel('SAVED'); toast('Saved '+r);
             if(pasarOn) pasarSendView(); }
      setTimeout(function(){ if(!hwRec) hwLabel('SHUTTER'); },1400);
    })
    .catch(function(){ hwBusy=false; hwLabel('SHUTTER'); toast('Capture failed',true); });
}
function hwStartRec(){
  hwHoldFired=true; hwNeedRelease=true;      /* ignore the release that follows */
  hwBusy=true; hwLabel('...');
  api('/rec/start').then(function(d){
    hwBusy=false;
    if(!d||!d.ok){ hwLabel('SHUTTER'); hwArc(0);
                   toast((d&&d.err)||'Cannot record',true); return; }
    hwRec=true;
    hwEl().classList.add('recording');
    hwLabel('REC');
    toast('Recording '+(d.file||''));
  }).catch(function(){ hwBusy=false; hwLabel('SHUTTER'); hwArc(0);
                       toast('Record failed',true); });
}
function hwStopRec(){
  hwBusy=true; hwLabel('...');
  api('/rec/stop').then(function(d){
    hwBusy=false; hwRec=false;
    hwEl().classList.remove('recording'); hwArc(0);
    hwLabel('SAVED');
    toast('Saved '+((d&&d.result)||''));
    setTimeout(function(){ if(!hwRec) hwLabel('SHUTTER'); },1600);
  }).catch(function(){ hwBusy=false; hwLabel('SHUTTER'); toast('Stop failed',true); });
}
(function(){
  var e=hwEl(); if(!e) return;
  e.addEventListener('pointerdown',hwPress);
  e.addEventListener('pointerup',hwRelease);
  e.addEventListener('pointercancel',hwRelease);
  e.addEventListener('pointerleave',function(ev){ if(hwDown) hwRelease(ev); });
  e.addEventListener('contextmenu',function(ev){ ev.preventDefault(); });
})();


function saveDevName(){
  var v=(document.getElementById('devname').value||'').trim();
  if(!v){ toast('Give it a name first',true); return; }
  fetch('/name?n='+encodeURIComponent(v)).then(function(r){return r.json()})
    .then(function(d){ if(d&&d.ok){ document.getElementById('devname').value=d.name;
                                    toast('This camera is now '+d.name); }
                       else toast('Rename failed',true); })
    .catch(function(){ toast('Rename failed',true); });
}
(function(){
  presetRefresh();
  var sel=document.getElementById('sel-fxscale');
  if(sel) sel.value=String(fxScale);
  fetch('/name').then(function(r){return r.json()})
    .then(function(d){ var e=document.getElementById('devname');
                       if(e&&d&&d.name) e.value=d.name; })
    .catch(function(){});
})();


/* ── presets ─────────────────────────────────────────────────────────────
   Every parameter the look depends on, gathered here in one place so saving
   and loading can never drift apart -- add a knob to this list and both
   directions pick it up. Written to the card, so a look found tonight is
   still there next month, and travels if the card does. */
function presetGather(){
  return {
    v:1, effect:fxN,
    amt:+fxAmt.toFixed(4), depth:+fxDepth.toFixed(4), speed:+fxSpeed.toFixed(4),
    grain:+fxGrain.toFixed(4), wet:+fxWet.toFixed(4),
    zoom:+fxZoom.toFixed(4), panX:+fxPanX.toFixed(4), panY:+fxPanY.toFixed(4),
    colA:fxCA.slice(0), colB:fxCB.slice(0),
    mask:maskMode, maskSrc:maskSrc, maskThresh:+maskThresh.toFixed(4),
    boxMode:boxMode, fxScale:fxScale, skin:(SKIN||'nacre'),
    accOn:(typeof accOn!=='undefined')?!!accOn:false,
    accK:(typeof accK!=='undefined')?+accK.toFixed(4):null
  };
}
function presetApply(d){
  if(!d||typeof d!=='object') return false;
  if(d.effect!=null){ fxN=d.effect;
    var sf=document.getElementById('sel-fx'); if(sf) sf.value=String(fxN); }
  if(d.amt!=null)   fxAmt=d.amt;
  if(d.depth!=null) fxDepth=d.depth;
  if(d.speed!=null) fxSpeed=d.speed;
  if(d.grain!=null) fxGrain=d.grain;
  if(d.wet!=null)   fxWet=d.wet;
  if(d.zoom!=null)  fxZoom=d.zoom;
  if(d.panX!=null)  fxPanX=d.panX;
  if(d.panY!=null)  fxPanY=d.panY;
  if(d.colA)        fxCA=d.colA.slice(0);
  if(d.colB)        fxCB=d.colB.slice(0);
  if(d.maskThresh!=null) maskThresh=d.maskThresh;
  if(d.maskSrc)     setMaskSrc(d.maskSrc);
  if(d.mask!=null)  setMaskMode(d.mask);
  if(d.boxMode!=null) setBoxMode(d.boxMode);
  if(d.skin)        setSkin(d.skin);
  if(d.fxScale!=null && d.fxScale!==fxScale) setFxScale(d.fxScale);
  if(typeof fxLabels==='function') fxLabels();
  if(typeof syncUI==='function') syncUI();
  return true;
}
function presetRefresh(){
  fetch('/presets',{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(a){
      var sel=document.getElementById('sel-preset');
      if(!sel) return;
      sel.innerHTML='<option value="">— saved looks —</option>';
      (a||[]).forEach(function(n){
        var o=document.createElement('option'); o.value=n; o.textContent=n;
        sel.appendChild(o);
      });
    }).catch(function(){});
}
function presetSave(){
  var el=document.getElementById('presetname');
  var n=(el.value||'').trim();
  if(!n){ toast('Name the look first',true); el.focus(); return; }
  fetch('/preset?n='+encodeURIComponent(n),
        {method:'POST',body:JSON.stringify(presetGather())})
    .then(function(r){return r.json()})
    .then(function(d){
      if(d&&d.ok){ toast('Saved "'+d.name+'" to the card'); el.value=''; presetRefresh(); }
      else toast('Save failed: '+((d&&d.err)||'?'),true);
    }).catch(function(){ toast('Save failed',true); });
}
function presetLoad(n){
  if(!n) return;
  fetch('/preset?n='+encodeURIComponent(n),{cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(d){
      if(d&&d.ok===false){ toast('Could not load',true); return; }
      if(presetApply(d)) toast('Loaded "'+n+'"');
    }).catch(function(){ toast('Could not load',true); });
}
function presetDelete(){
  var sel=document.getElementById('sel-preset');
  var n=sel?sel.value:'';
  if(!n){ toast('Pick a look to remove',true); return; }
  fetch('/preset/del?n='+encodeURIComponent(n)).then(function(r){return r.json()})
    .then(function(d){ toast(d&&d.ok?('Removed "'+n+'"'):'Not found',!(d&&d.ok));
                       presetRefresh(); }).catch(function(){});
}


/* ── THE RIG ─────────────────────────────────────────────────────────────
   Peers announce themselves; this only lists what the device already heard.
   A nearby-radio peer can join BWO without an IP address. Photo transfer and
   opening its web page require both cameras to share a Wi-Fi network. */
var rigPeers=[];
function rigPoll(){
  fetch('/link/peers',{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(d){
      if(!d||!d.ok) return;
      rigPeers=d.peers||[];
      var el=document.getElementById('rigwho');
      if(!el) return;
      if(!rigPeers.length){ el.textContent = d.up ? 'no other cameras yet'
                                                  : 'mesh off'; return; }
      var h='';
      for(var i=0;i<rigPeers.length;i++){
        var p=rigPeers[i];
        h+='<div style="margin:3px 0">&#9678; <b>'+esc(p.name)+'</b> '
         + '<span style="opacity:.6">'+(p.ip&&p.ip!=='0.0.0.0'?esc(p.ip):'nearby radio')
         + ' &middot; '+p.photos+' frames'+(p.playing?' &middot; BWO active':'')+'</span> '
         + (p.ip&&p.ip!=='0.0.0.0'
            ? '<a href="#" onclick="rigOpen('+i+');return false">roll</a> '
             + '<a href="#" onclick="rigPull('+i+');return false">pull latest</a>'
            : '')+'</div>';
      }
      el.innerHTML=h;
      var ps=document.getElementById('rigpull');
      if(ps&&d.pull&&d.pull!=='idle') ps.textContent='copy: '+d.pull;
    }).catch(function(){});
}
function esc(t){ return String(t==null?'':t).replace(/[<>&"]/g,function(c){
  return {'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;'}[c]; }); }
function rigOpen(i){
  var p=rigPeers[i]; if(!p) return;
  window.open('http://'+p.ip+'/','_blank');
}
function rigPull(i){
  var p=rigPeers[i]; if(!p) return;
  if(!p.photos){ toast('That camera has no frames yet',true); return; }
  fetch('/link/pull?ip='+encodeURIComponent(p.ip)+'&n='+(p.photos-1)
        +'&as='+encodeURIComponent(p.name),{cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(d){ toast(d&&d.ok?('Copying from '+p.name+'\u2026')
                                    :('Busy: '+((d&&d.status)||'?')),!(d&&d.ok)); })
    .catch(function(){ toast('Could not reach the mesh',true); });
}
function rigFire(){
  fetch('/link/fire?lead=400&tag='+encodeURIComponent(myName||'rig'),{cache:'no-store'})
    .then(function(r){return r.json()})
    .then(function(d){
      if(!d||!d.ok){ toast('Mesh is off',true); return; }
      toast(d.peers?('Firing '+(d.peers+1)+' cameras\u2026'):'Firing (no peers heard)');
    }).catch(function(){ toast('Could not fire',true); });
}

/* ── PRE-ROLL ─────────────────────────────────────────────────────────── */
function preShow(d){
  var el=document.getElementById('prestat'); if(!el||!d) return;
  var b=document.getElementById('btn-prearm');
  if(!d.on){ el.textContent='off \u2014 '+Math.round(d.psramFree/1024)+' KB free to use';
             if(b) b.textContent='ARM'; return; }
  if(b) b.textContent='RELEASE';
  el.textContent='holding '+d.held+' frames'
    + (d.spanMs?(' \u2014 '+(d.spanMs/1000).toFixed(1)+' s'):'')
    + ' \u2014 '+d.kb+' KB reserved';
}
function preArm(){
  var b=document.getElementById('btn-prearm');
  var on=(b&&b.textContent==='RELEASE')?0:1;
  fetch('/preroll?on='+on+'&slots=24',{cache:'no-store'})
    .then(function(r){return r.json()}).then(function(d){
      preShow(d);
      toast(d.on?('Pre-roll armed \u2014 '+d.slots+' frames'):'Pre-roll released');
    }).catch(function(){ toast('Could not arm',true); });
}
function preSave(){
  fetch('/preroll/save',{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(d){
      toast(d&&d.ok?('Saved '+d.frames+' frames from before the press')
                   :'Nothing held yet',!(d&&d.ok));
      prePoll();
    }).catch(function(){ toast('Save failed',true); });
}
function prePoll(){
  fetch('/preroll',{cache:'no-store'}).then(function(r){return r.json()})
    .then(preShow).catch(function(){});
}
setInterval(function(){ rigPoll(); prePoll(); },3000);

function setBoxMode(v){
  boxMode=parseInt(v,10)||0;
  if(boxMode>0){
    if(maskMode===0) setMaskMode(1);            // boxes come from the mask
    if(maskSrc==='off') setMaskSrc('motion');
  }
  toast(boxMode?('Tracking '+['','inside','outside','outline'][boxMode]):'Tracking off');
}
/* ── EXPOSURE ENGINE ──────────────────────────────────────────────
   The shutter is a duration, not an instant. Every rendered frame is
   composited into an accumulator with a chosen blend, so the photograph is
   built over time: LIGHTEN paints with light, AVERAGE is a true long
   exposure, DARKEN keeps only what stays still, SLIT maps time onto the
   vertical axis (each row is a different moment). */
var accFbo=[null,null],accT=[null,null],accPP=0,accOn=false,accMode=90,
    accN=0,accK=1,accFrozen=false,accStart=0,accDur=12000;
function hx2rgb(h){return [parseInt(h.substr(1,2),16)/255,
                            parseInt(h.substr(3,2),16)/255,
                            parseInt(h.substr(5,2),16)/255];}
/* Each look uses the three generic sliders differently. Relabelling them per
   effect turns "Amount/Depth/Speed" into controls you can actually reason about. */
var FXP={
 1:['Trail','--','Rotation'],       2:['Intensity','Radius','--'],
 3:['Split','--','--'],             4:['Warp','Scale','Drift'],
 5:['Threshold','Kernel','--'],     6:['Levels','--','--'],
 7:['Damage','--','Jitter'],        8:['Blend','--','--'],
 9:['Cell size','Bevel','--'],      10:['Light angle','Height','Orbit'],
 11:['Tilt','Z depth','Spin'],      12:['Block size','Extrude','Orbit'],
 13:['Segments','Warp','Spin'],     14:['Dot scale','--','--'],
 15:['Line count','Thickness','--'],16:['Hold','Rows/frame','--'],
 17:['Barrel','--','--'],           18:['Smear','Distance','Direction'],
 20:['Glow','Radius','--'],         21:['Halation','Radius','--'],
 22:['Streak','Length','--'],       23:['Diffusion','Radius','--'],
 24:['Blur','Focus band','--'],     25:['Bypass','--','--'],
 26:['Grade','--','--'],            27:['Night','--','--'],
 28:['Cross','--','--'],            30:['Gamma','Smooth','--'],
 31:['Strength','Sample','--'],     32:['Density','--','Shift'],
 33:['Leak','Falloff','Drift'],     40:['Blend','Gamma','--'],
 41:['Solarize','Threshold','--'],  42:['Twist','Frequency','Speed'],
 43:['Height','Frequency','Speed'], 44:['Distance','Bias','--'],
 45:['Scanline','Curvature','--'],  46:['Blend','Axis mode','--'],
 47:['Posterize','Outline','--'],   48:['Blend','Aperture','--'],
 49:['Corruption','Row count','Rate'],
 56:['Stroke','Length','--'],       57:['Refraction','Scale','Flow'],
 58:['Relief','Depth','Sway'],      59:['Ink','Screen ruling','--'],
 60:['Trail','Drift','--']
};
function fxLabels(){
  var L=FXP[fxN]||['Amount','Depth','Speed'];
  document.getElementById('lb-a').textContent=L[0];
  document.getElementById('lb-d').textContent=L[1];
  document.getElementById('lb-s').textContent=L[2];
}
function setCol(){
  fxCA=hx2rgb(document.getElementById('colA').value);
  fxCB=hx2rgb(document.getElementById('colB').value);
}
function pal(a,b){
  document.getElementById('colA').value=a;
  document.getElementById('colB').value=b;
  setCol();
}
function hsl2rgb(h,sl,l){
  h/=360;var q=l<.5?l*(1+sl):l+sl-l*sl,pp=2*l-q;
  function f(t){t=(t+1)%1;
    if(t<1/6)return pp+(q-pp)*6*t;
    if(t<1/2)return q;
    if(t<2/3)return pp+(q-pp)*(2/3-t)*6;
    return pp;}
  return [f(h+1/3),f(h),f(h-1/3)];
}
function hueSet(which,val){
  var rgb=hsl2rgb(parseInt(val,10),.75,.62);
  if(which===0){fxCA=rgb;sv('vha',val);} else {fxCB=rgb;sv('vhb',val);}
}
function swapCol(){
  var a=document.getElementById('colA').value;
  pal(document.getElementById('colB').value,a);
}
/* Upload a mask image (canvas or ImageBitmap) as the gating texture. */
function maskUpload(src){
  if(!gl)return;
  if(!maskTex) maskTex=mkTex();
  gl.activeTexture(gl.TEXTURE2);
  gl.bindTexture(gl.TEXTURE_2D,maskTex);
  gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,true);
  try{ gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,src); }catch(e){}
}
/* Placeholder subject: a soft centre ellipse. This exists to prove the gating
   path end to end — it is NOT recognition. A real segmentation model writes to
   exactly this canvas and nothing else changes. */
function maskPlaceholder(){
  if(!maskCv){ maskCv=document.createElement('canvas'); maskCv.width=160; maskCv.height=120;
               maskCx=maskCv.getContext('2d'); }
  var w=maskCv.width,h=maskCv.height;
  maskCx.fillStyle='#000'; maskCx.fillRect(0,0,w,h);
  var g=maskCx.createRadialGradient(w/2,h*0.55,4,w/2,h*0.55,h*0.46);
  g.addColorStop(0,'#fff'); g.addColorStop(.7,'#fff'); g.addColorStop(1,'#000');
  maskCx.fillStyle=g;
  maskCx.beginPath(); maskCx.ellipse(w/2,h*0.55,w*0.24,h*0.42,0,0,7); maskCx.fill();
  maskUpload(maskCv);
}
function setMaskMode(v){
  maskMode=parseInt(v,10)||0;
  if(maskMode>0 && maskSrc==='off'){ setMaskSrc('motion'); }
  toast(maskMode?('Applying to '+['','subject','background','outline'][maskMode]):'Whole frame');
}
function setMaskSrc(v){
  maskSrc=v;
  if(v==='motion'){ if(gl){ maskAlloc(); } maskTex=mTex;
                    toast('Learning the scene — hold still a moment'); }
  else if(v==='placeholder'){ maskPlaceholder(); }
  else { maskTex=null; }
}
function relearnBg(){ bgReady=0; toast('Relearning the background'); }
function fxSrc(){return (gl&&glReady)?gc:img}
/* The final pass is the plain copy unless the person asked for something: no effect, no exposure
   build-up and no grain slider means the camera's own picture reaches the screen untouched. The
   grade-and-grain pass (98) only runs when it was asked for. */
function finalFx(){ return (fxN===0&&!accOn&&fxGrain<=0.001)?99:98; }

/* ── Which way up the picture goes ─────────────────────────────────────────
   The sensor is mounted so a portrait scene arrives sideways; the camera's own screen has always
   turned it clockwise by default, and the phone view showed it unturned, 90 degrees to the left of
   what the camera shows. viewRot is that same setting (clockwise quarter turns), kept on the
   camera so the screen, this page, saved photos (EXIF) and recordings (track matrix) agree. The
   turn is done by the browser's compositor: nothing is re-encoded and no frame is copied. */
var viewRot=1;
function viewTurn(){ return viewRot&3; }
var _vfit={r:-1,w:0,h:0,iw:0,ih:0};
function applyViewRot(){
  var sv=document.querySelector('.sv'), iw=img.naturalWidth, ih=img.naturalHeight;
  var b=document.getElementById('btn-rot');
  if(b) b.textContent='VIEW ROTATE '+(viewTurn()*90)+'\u00b0';
  if(!sv||!iw||!ih) return;
  var q=viewTurn()&1, deg=viewTurn()*90;
  var k=Math.min(sv.clientWidth/(q?ih:iw), sv.clientHeight/(q?iw:ih));
  if(!(k>0)) return;
  var w=Math.floor(iw*k)+'px', h=Math.floor(ih*k)+'px';
  _vfit={r:viewTurn(),w:sv.clientWidth,h:sv.clientHeight,iw:iw,ih:ih};
  img.style.maxWidth='none'; img.style.maxHeight='none'; img.style.width=w; img.style.height=h;
  img.style.transform='rotate('+deg+'deg) scale('+fxZoom.toFixed(3)+')';
  gc.style.maxWidth='none'; gc.style.maxHeight='none'; gc.style.width=w; gc.style.height=h;
  gc.style.transform='translate(-50%,-50%) rotate('+deg+'deg)';
  gc._vr=1;
}
function viewRotStale(){
  var sv=document.querySelector('.sv');
  return !gc._vr||_vfit.r!==viewTurn()||!sv||_vfit.w!==sv.clientWidth||_vfit.h!==sv.clientHeight||
         _vfit.iw!==img.naturalWidth||_vfit.ih!==img.naturalHeight;
}
function cycleViewRot(){
  viewRot=(viewRot+1)&3; applyViewRot();
  api('/set?viewrot='+viewRot);
  toast('View rotated '+(viewTurn()*90)+'\u00b0');
}
window.addEventListener('resize',function(){ applyViewRot(); });
if(window.ResizeObserver){ var _svq=document.querySelector('.sv'); if(_svq) new ResizeObserver(function(){ applyViewRot(); }).observe(_svq); }

/* A copy of a canvas turned by n clockwise quarter turns, for anything that is saved or sent. */
function rotated(src,n){
  n&=3; if(!n) return src;
  var q=n&1, c=document.createElement('canvas');
  c.width=q?src.height:src.width; c.height=q?src.width:src.height;
  var x=c.getContext('2d');
  x.translate(c.width/2,c.height/2); x.rotate(n*Math.PI/2);
  x.drawImage(src,-src.width/2,-src.height/2);
  return c;
}
/* Quarter turns a JPEG's EXIF Orientation asks for (0 when it has none). Browsers apply that tag
   themselves when they decode the picture, so a still that carries it must not be turned again. */
function exifTurns(buf){
  try{
    var v=new DataView(buf); if(v.byteLength<12||v.getUint16(0)!==0xFFD8) return 0;
    var o=2;
    while(o+4<=v.byteLength){
      var m=v.getUint16(o); if((m&0xFF00)!==0xFF00||m===0xFFDA) return 0;
      if(m===0xFFE1&&o+14<=v.byteLength&&v.getUint32(o+4)===0x45786966){
        var t=o+10, le=v.getUint16(t)===0x4949, ifd=t+v.getUint32(t+4,le), n=v.getUint16(ifd,le);
        for(var i=0;i<n;i++){
          var e=ifd+2+i*12; if(e+12>v.byteLength) break;
          if(v.getUint16(e,le)===0x0112){ var r=v.getUint16(e+8,le); return r===6?1:r===3?2:r===8?3:0; }
        }
        return 0;
      }
      o+=2+v.getUint16(o+2);
    }
  }catch(e){}
  return 0;
}
function glDie(msg,fatal){
  if(msg){toast('GPU FX off: '+msg,true);rlog('GLDIE '+msg);}
  if(fatal)glFatal=true;      // shader/link failure: do not keep retrying
  gl=null;glReady=false;
  gc.classList.remove('on');img.classList.remove('dim');
}
/* ── The GL context can be taken away, and the page has to cope ───────────
   iOS Safari drops a page's WebGL context whenever the tab is backgrounded:
   the share sheet, a trip to Photos to look at a shot, the screen locking, or
   plain memory pressure. Two things then went wrong here.

   1. The loss event was never cancelled. Per the spec a lost context is only
      eligible for restoration if the page calls preventDefault() on
      'webglcontextlost'. Nothing did, so the browser was told the loss was
      final: isContextLost() stayed true even after restoreContext().
   2. Nothing rebuilt anything afterwards. Every texture, framebuffer and
      program was a handle into a dead context, `gl` was still a live object,
      and glReady still said true -- so the page kept calling into a context
      that was gone and the whole picture went black, until the page was
      reloaded by hand.

   Reproduced in the iOS 18.3 Simulator with WEBGL_lose_context: after a
   lose/restore cycle the canvas was 100% black with CONTEXT_LOST_WEBGL and
   INVALID_OPERATION on every call.

   The recovery drops every handle (they all belong to the old context) and
   lets the existing lazy allocation rebuild them: glInit() remakes the
   programs, the next render() sees the canvas size no longer matches and calls
   glSize(), and the mask, box, scene and exposure buffers are all created on
   first use. The plain <img> stays on screen meanwhile, so the viewfinder never
   goes black while this happens. */
var glLost=false, glLostTimer=0;
function glForget(){
  glP=null; glU={}; glTex=null; fbo=[null,null]; fbt=[null,null]; pp=0;
  quadBuf=null; pcP=null; pcU={}; pcBuf=null; pcN=0;
  bgFbo=[null,null]; bgTex=[null,null]; bgPP=0; bgReady=0; mFbo=null; mTex=null;
  if(maskSrc==='motion') maskTex=null;
  bFbo=null; bTex=null; bPix=null; sFbo=null; sTex=null; sPix=null;
  accFbo=[null,null]; accT=[null,null]; accPP=0;
}
function glRebuild(why){
  rlog('GL rebuild: '+why);
  glForget();
  glReady=false; texCount=0; texDirty=true;
  gc.classList.remove('on'); img.classList.remove('dim');     // plain picture meanwhile
  try{ glInit(); }catch(e){ glDie('rebuild '+((e&&e.name)||'err')); return; }
  if(gl){ gc.width=1; gc.height=1; }       // size mismatch makes render() call glSize()
}
function glWatch(c){
  c.addEventListener('webglcontextlost',function(e){
    e.preventDefault();                    // "I want it back" -- required for restore
    glLost=true; glReady=false;
    c.classList.remove('on'); img.classList.remove('dim');
    rlog('GL context lost');
  },false);
  c.addEventListener('webglcontextrestored',function(){
    glLost=false; clearTimeout(glLostTimer);
    glRebuild('context restored');
  },false);
}
/* Last resort: a context that stays lost cannot be revived on its own canvas,
   but a fresh canvas element gets a fresh context. */
function glReplaceCanvas(){
  var n=gc.cloneNode(false);
  n.classList.remove('on');
  gc.parentNode.replaceChild(n,gc);
  gc=n; glWatch(gc); glLost=false;
  gl=null; glRebuild('replaced canvas');
}
function glCheckLost(){
  if(gl&&gl.isContextLost()){
    glLost=true; glReady=false;
    clearTimeout(glLostTimer);
    glLostTimer=setTimeout(function(){ if(gl&&gl.isContextLost()) glReplaceCanvas(); },2500);
  }
}
var FSRC=
'precision highp float;varying vec2 v;'+
'uniform sampler2D T;uniform sampler2D P;'+
'uniform float t;uniform int fx;uniform float amt;uniform float dep;'+
'uniform float spd;uniform float grn;uniform float k;uniform vec2 R;'+
'uniform vec3 cA;uniform vec3 cB;uniform float wet;'+
'uniform float zm;uniform vec2 pn;'+
'uniform sampler2D M;uniform int mmode;'+   /* subject mask + how to use it */
'uniform vec4 bx[8];uniform int nbx;uniform int bmode;'+ /* tracked object boxes */
'vec2 zc(vec2 p){return (p-.5)/zm+.5+pn;}'+
/* ── helpers ── */
'float h1(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}'+
'float n2(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);'+
'return mix(mix(h1(i),h1(i+vec2(1,0)),f.x),mix(h1(i+vec2(0,1)),h1(i+vec2(1,1)),f.x),f.y);}'+
'float fbm(vec2 p){float s=0.,a=.5;for(int i=0;i<4;i++){s+=a*n2(p);p*=2.03;a*=.5;}return s;}'+
'float lum(vec3 c){return dot(c,vec3(.299,.587,.114));}'+
'float lu(vec2 o){return lum(texture2D(T,zc(v)+o/R).rgb);}'+
'float bay2(vec2 a){a=floor(a);return fract(a.x*.5+a.y*a.y*.75);}'+
'vec3 sat(vec3 c,float s){float l=lum(c);return mix(vec3(l),c,s);}'+
/* one rotated halftone screen: dot area grows with ink coverage */
'float scrn(vec2 p,float f,float a,float ink){'+
'mat2 r=mat2(cos(a),-sin(a),sin(a),cos(a));'+
'vec2 cc=fract(r*p*f)-.5;'+
'return 1.-smoothstep(ink-.18,ink+.18,length(cc)*2.);}'+
'vec3 bl13(vec2 p,float r){vec3 s=texture2D(T,p).rgb;'+
'for(int i=0;i<12;i++){float a=float(i)*.5236;float rr=(mod(float(i),3.)+1.)/3.;'+
's+=texture2D(T,p+vec2(cos(a),sin(a))*r*rr/R).rgb;}return s/13.;}'+
'vec3 scr(vec3 a,vec3 b){return 1.-(1.-a)*(1.-b);}'+
'void main(){vec2 uv=zc(v);vec3 c=texture2D(T,uv).rgb;vec3 orig=c;'+
/* ── 99 = pure copy ── */
'if(fx==99){gl_FragColor=vec4(c,1.);return;}'+
/* ── 98 = final grade + fine film grain (two-octave, film response) ── */
'if(fx==90){gl_FragColor=vec4(max(c,texture2D(P,uv).rgb),1.);return;}'+
'if(fx==91){gl_FragColor=vec4(mix(texture2D(P,uv).rgb,c,k),1.);return;}'+
'if(fx==92){gl_FragColor=vec4(min(c,texture2D(P,uv).rgb),1.);return;}'+
'if(fx==93){vec3 pv=texture2D(P,uv).rgb;'+
'float band=1.-smoothstep(0.,2.5/R.y,abs(uv.y-k));'+
'gl_FragColor=vec4(mix(pv,c,band),1.);return;}'+
'if(fx==94){'+                                   /* learn the scene slowly  */
'vec3 bg=texture2D(P,uv).rgb;'+
'gl_FragColor=vec4(mix(bg,c,k),1.);return;}'+
'if(fx==95){'+                                   /* subject = not-background */
'vec3 bg=texture2D(P,uv).rgb;'+
'vec3 cd=abs(c-bg);'+
'float d=max(abs(lum(c)-lum(bg)),max(cd.r,max(cd.g,cd.b)));'+
'float m=smoothstep(amt,amt+.055,d);'+
'float n=0.;'+                                   /* despeckle: 4-neighbour vote */
'for(int i=0;i<4;i++){'+
'vec2 o=vec2(i==0?2.:(i==1?-2.:0.),i==2?2.:(i==3?-2.:0.))/R;'+
'vec3 c2=texture2D(T,uv+o).rgb;vec3 b2=texture2D(P,uv+o).rgb;'+
'n+=step(amt,max(abs(lum(c2)-lum(b2)),max(abs(c2.r-b2.r),abs(c2.g-b2.g))));}'+
'gl_FragColor=vec4(vec3(m*step(1.5,n+m*2.)),1.);return;}'+
'if(fx==98){float L=lum(c);'+
'float g1=h1(uv*R+vec2(mod(t,13.)*197.,mod(t,17.)*331.))-.5;'+
'float g2=n2(uv*R*.4+vec2(mod(t,7.)*53.,mod(t,11.)*97.))-.5;'+
'float gg=g1*.8+g2*.5;'+
'float w=(1.-.7*L)*(.3+.7*smoothstep(0.,.22,L));'+
'c+=gg*grn*.9*w;'+
'c=clamp(c,0.,1.);'+
'c=c*c*(3.-2.*c)*.12+c*.88;'+          /* gentle S-curve for contrast */
'gl_FragColor=vec4(clamp(c,0.,1.),1.);return;}'+
/* ── 1 Feedback Trails — true Feedback TOP: rotate+zoom prev OUTPUT ── */
'if(fx==1){vec2 q=uv-.5;q.x*=R.x/R.y;'+
'float an=(.14+.5*amt)*sin(t*.23*spd)*.6;float zm=1.-(.006+.03*amt);'+
'vec2 f=vec2(q.x*cos(an)-q.y*sin(an),q.x*sin(an)+q.y*cos(an))*zm;'+
'f.x/=R.x/R.y;f+=.5;'+
'vec3 fb=texture2D(P,f).rgb;'+
'fb=sat(fb,1.06)*(.9+.085*amt);'+       /* slight sat bloom per generation */
'c=max(c,fb*smoothstep(-.05,.35,lum(fb)));}'+
/* ── 2 Bloom — separable-ish gaussian, soft-knee threshold ── */
'else if(fx==2){vec3 b=vec3(0.);float wsum=0.;'+
'for(int i=-3;i<=3;i++){for(int j=-3;j<=3;j++){'+
'vec2 o=vec2(float(i),float(j));float wg=exp(-dot(o,o)*.22);'+
'vec3 s=texture2D(T,uv+o*(2.2+3.*dep)/R).rgb;'+
'float k=smoothstep(.5,.92,lum(s));'+
'b+=s*k*wg;wsum+=wg;}}'+
'b/=wsum;c=1.-(1.-c)*(1.-b*2.6*amt);}'+  /* screen blend, no clipping */
/* ── 3 RGB Split — radial CA with barrel, edge-weighted ── */
'else if(fx==3){vec2 p=uv-.5;float r2=dot(p,p);'+
'vec2 d=p*(.02+.09*amt)*(.35+r2*2.2);'+
'c=vec3(texture2D(T,uv+d*1.0).r,texture2D(T,uv+d*.15).g,texture2D(T,uv-d*.85).b);'+
'c*=1.-r2*.35*amt;}'+
/* ── 4 Noise Displace — FBM domain warp (Noise TOP → Displace TOP) ── */
'else if(fx==4){float T1=t*.35*spd;'+
'vec2 w1=vec2(fbm(uv*3.2+vec2(T1,0.)),fbm(uv*3.2+vec2(5.2,T1)))-.5;'+
'vec2 w2=vec2(fbm(uv*9.-w1*2.+T1*.7),fbm(uv*9.+w1*2.+11.-T1*.5))-.5;'+
'c=texture2D(T,uv+(w1*.75+w2*.3)*(.05+.2*amt)*(.4+dep)).rgb;}'+
/* ── 5 Edge Detect — Sobel + soft glow over graded base ── */
'else if(fx==5){float sc=1.+2.*dep;'+
'float gx=lu(vec2(-sc,-sc))+2.*lu(vec2(-sc,0.))+lu(vec2(-sc,sc))'+
'-lu(vec2(sc,-sc))-2.*lu(vec2(sc,0.))-lu(vec2(sc,sc));'+
'float gy=lu(vec2(-sc,-sc))+2.*lu(vec2(0.,-sc))+lu(vec2(sc,-sc))'+
'-lu(vec2(-sc,sc))-2.*lu(vec2(0.,sc))-lu(vec2(sc,sc));'+
'float e=length(vec2(gx,gy));'+
'float ed=smoothstep(.05,.05+.7*(1.-amt),e);'+
'vec3 tint=mix(cA,cB,smoothstep(.22,.92,lum(c)));'+
'c=c*(.10+.30*(1.-amt))+tint*ed*(.7+.9*amt);}'+
/* ── 6 Dither Posterize — ordered Bayer, riso/print look ── */
'else if(fx==6){float lv=mix(12.,2.5,amt);'+
'vec2 bp=uv*R;'+
'float b4=bay2(bp*.5)*.25+bay2(bp);'+
'float b8=(bay2(bp*.25)*.25+b4)*.8;'+
'c=floor(c*lv+(b8-.5)*1.15+.5)/lv;'+
'c=sat(c,1.15);}'+
/* ── 7 VHS — tracking wobble, head-switch tear, chroma bleed, dropout ── */
'else if(fx==7){'+
'float band=floor(uv.y*28.);'+
'float jit=(n2(vec2(band,t*2.4))-.5)*.010*amt;'+
'float hsw=(1.-smoothstep(0.,.06,uv.y))*(n2(vec2(t*8.,uv.y*90.))-.5)*.16*amt;'+
'float tear=step(.988,n2(vec2(floor(t*7.),band)))*(h1(vec2(band,t))-.5)*.22*amt;'+
'vec2 wu=uv+vec2(jit+hsw+tear,0.);'+
'float ca=(.0016+.004*amt);'+
'vec3 cy=vec3(texture2D(T,wu+vec2(ca,0.)).r,texture2D(T,wu).g,texture2D(T,wu-vec2(ca,0.)).b);'+
'vec3 bl=vec3(0.);for(int i=-3;i<=3;i++){bl+=texture2D(T,wu+vec2(float(i)*3.5/R.x,0.)).rgb;}'+
'bl/=7.;cy=vec3(cy.r,mix(cy.g,bl.g,.35*amt),mix(cy.b,bl.b,.6*amt));'+  /* chroma bleeds, luma stays */
'cy*=.86+.14*sin(uv.y*R.y*3.14159);'+
'cy+=(h1(uv*R+t*137.)-.5)*.09*amt;'+
'cy*=1.-.28*amt*smoothstep(.4,1.,length((uv-.5)*vec2(1.1,1.)));'+
'c=sat(cy,1.-.35*amt);}'+
/* ── 8 Thermal LUT — smooth 6-stop false colour ── */
'else if(fx==8){float L=clamp(pow(lum(c),mix(1.,.72,amt))*(1.+.5*amt),0.,1.);'+
'vec3 k=mix(vec3(.01,.01,.10),vec3(.30,.02,.55),smoothstep(0.,.22,L));'+
'k=mix(k,vec3(.85,.05,.42),smoothstep(.22,.44,L));'+
'k=mix(k,vec3(1.,.32,.02),smoothstep(.44,.63,L));'+
'k=mix(k,vec3(1.,.78,.05),smoothstep(.63,.82,L));'+
'k=mix(k,vec3(1.,1.,.92),smoothstep(.82,1.,L));'+
'c=mix(c,k,clamp(amt*1.5,0.,1.));}'+
/* ── 9 Pixelate — cell quantise with subtle bevel grid ── */
'else if(fx==9){float cs=mix(220.,12.,amt);'+
'vec2 g=vec2(cs,cs*R.y/R.x);vec2 ci=floor(uv*g),cf=fract(uv*g);'+
'c=texture2D(T,(ci+.5)/g).rgb;'+
'float bv=smoothstep(0.,.14,cf.x)*smoothstep(0.,.14,cf.y);'+
'c*=mix(1.,.82+.36*bv,dep);}'+
/* ── 10 Relief 3D — parallax-occlusion heightfield + phong (3D recon) ── */
'else if(fx==10){float D=(.04+.20*dep);'+
'float a=t*.35*spd;'+
'vec2 vd=vec2(cos(a),sin(a*.7))*(.25+.75*amt);'+
'vec2 st=vd*D/24.;float hh=1.;vec2 pu=uv;float found=0.;'+
'for(int i=0;i<24;i++){'+
'float hs=lum(texture2D(T,pu).rgb);'+
'float hit=step(hh,hs)*(1.-found);found=max(found,step(hh,hs));'+
'pu+=st*(1.-found);hh-=(1./24.)*(1.-found);}'+
'vec3 col=texture2D(T,pu).rgb;'+
'float e=1.6/R.x;'+
'float hx=lum(texture2D(T,pu+vec2(e,0.)).rgb)-lum(texture2D(T,pu-vec2(e,0.)).rgb);'+
'float hy=lum(texture2D(T,pu+vec2(0.,e)).rgb)-lum(texture2D(T,pu-vec2(0.,e)).rgb);'+
'vec3 nr=normalize(vec3(-hx*(14.+40.*dep),-hy*(14.+40.*dep),1.));'+
'vec3 Lg=normalize(vec3(cos(a+1.1)*.8,sin(a+1.1)*.8,.75));'+
'float df=max(dot(nr,Lg),0.);'+
'float sp=pow(max(dot(reflect(-Lg,nr),vec3(0.,0.,1.)),0.),34.);'+
'float ao=clamp(.55+.55*nr.z,0.,1.);'+
'c=col*(.30+.85*df)*ao+vec3(.55,.75,1.)*sp*(.35+.5*amt);}'+
/* ── 12 Voxel Extrude — luma-height columns, top/side shading ── */
'else if(fx==12){float cs=mix(120.,26.,amt);'+
'vec2 g=vec2(cs,cs*R.y/R.x);'+
'float a=t*.28*spd;vec2 dir=vec2(cos(a),sin(a)*.6)*(.05+.16*dep);'+
'vec3 res=texture2D(T,(floor(uv*g)+.5)/g).rgb*.14;float got=0.;'+
'for(int i=0;i<20;i++){float s=(19.-float(i))/19.;'+
'vec2 ci=floor((uv+dir*s)*g);'+
'vec3 sc=texture2D(T,(ci+.5)/g).rgb;'+
'float on=step(s,lum(sc))*(1.-got);'+
'float top=step(s,lum(sc))*step(lum(sc)-.055,s);'+
'res=mix(res,sc*(.48+.52*(1.-s*.8))+vec3(.22)*top,on);'+
'got=max(got,step(s,lum(sc)));}'+
'c=res;}'+
/* ── 13 Kaleidoscope — radial mirror segments, slow spin ── */
'else if(fx==13){vec2 p=uv-.5;p.x*=R.x/R.y;'+
'float an=atan(p.y,p.x)+t*.12*spd,r=length(p);'+
'float seg=floor(mix(3.,14.,amt));float sa=6.28318/seg;'+
'an=abs(mod(an,sa)-sa*.5);'+
'r*=mix(1.,.82+.36*sin(r*10.-t*.5*spd),dep*.5);'+
'vec2 q=vec2(cos(an),sin(an))*r;q.x/=R.x/R.y;'+
'c=texture2D(T,clamp(q+.5,.002,.998)).rgb;}'+
/* ── 14 Halftone — rotated CMY dot screens over K, print emulation ── */
'else if(fx==14){float sc=mix(260.,55.,amt);'+
'vec3 cmy=1.-c;'+
'vec3 o=vec3(0.);'+
'for(int ch=0;ch<3;ch++){'+
'float ang=float(ch)*.3927+.2618;'+
'vec2 rp=vec2(uv.x*cos(ang)-uv.y*sin(ang),uv.x*sin(ang)+uv.y*cos(ang));'+
'vec2 gp=rp*vec2(sc,sc*R.y/R.x);'+
'float val=ch==0?cmy.r:(ch==1?cmy.g:cmy.b);'+
'float d=length(fract(gp)-.5);'+
'float dot_=1.-smoothstep(sqrt(val)*.72-.09,sqrt(val)*.72,d);'+
'if(ch==0)o.r=dot_;else if(ch==1)o.g=dot_;else o.b=dot_;}'+
'c=mix(c,1.-o,clamp(amt*1.4,0.,1.));}'+
/* ── 15 Contour — topographic iso-lines from luminance ── */
'else if(fx==15){float N=mix(6.,30.,amt);'+
'vec3 sm=bl13(uv,.8+2.2*dep);float L=lum(sm);'+
'float q=L*N;float w=.028+.085*(1.-dep);'+
'float ln=1.-smoothstep(0.,w,abs(fract(q)-.5));'+
'float maj=1.-smoothstep(0.,w*1.7,abs(fract(q*.2)-.5));'+
'vec3 base=mix(vec3(0.),c*.5,1.-amt*.78);'+
'c=base+cA*ln*.95+cB*maj*.8;}'+
/* ── 16 Slit Scan — feedback row-scroll time smear ── */
'else if(fx==16){float sp2=1.+floor(3.*dep);'+
'vec3 pr=texture2D(P,uv-vec2(0.,sp2/R.y)).rgb;'+
'float head=1.-smoothstep(0.,sp2*2./R.y,uv.y);'+
'c=mix(pr,c,clamp(head+(1.-amt)*.06,0.,1.));}'+
/* ── 17 Chroma Fisheye — barrel + per-channel scale + vignette ── */
'else if(fx==17){vec2 p=uv-.5;float r=length(p);'+
'vec2 d=p*(1.+(.35+1.35*amt)*r*r);'+
'float s=.0024*amt+.0012;'+
'c=vec3(texture2D(T,.5+d*(1.-s)).r,texture2D(T,.5+d).g,texture2D(T,.5+d*(1.+s)).b);'+
'c*=mix(1.,1.-smoothstep(.28,.92,r),amt);'+
'c=sat(c,1.+.25*amt);}'+
/* ── 18 Optical Smear — directional feedback advection ── */
'else if(fx==18){float a=t*.19*spd;'+
'vec2 fl=vec2(cos(a),sin(a))*(.001+.006*dep);'+
'vec3 pr=texture2D(P,uv+fl).rgb;'+
'pr=sat(pr,1.03);'+
'c=mix(c,max(pr*.985,c*.55),.55+.42*amt);}'+
'else if(fx==20){vec3 b=bl13(uv,5.+20.*dep);'+
'vec3 g=scr(b,b);c=mix(c,scr(c,g),.45+.55*amt);c*=1.+.06*amt;}'+
'else if(fx==21){vec3 b=bl13(uv,7.+26.*dep);'+
'float m=smoothstep(.55,1.,lum(b));'+
'c+=b*m*vec3(1.,.30,.10)*1.7*amt;}'+
'else if(fx==22){vec3 st=vec3(0.);float w=0.;'+
'for(int i=-10;i<=10;i++){float o=float(i);float g=exp(-o*o*.022);'+
'vec3 sm=texture2D(T,uv+vec2(o*(3.+10.*dep)/R.x,0.)).rgb;'+
'st+=sm*smoothstep(.60,1.,lum(sm))*g;w+=g;}st/=w;'+
'c+=st*vec3(.22,.52,1.65)*2.3*amt;}'+
'else if(fx==23){vec3 b=bl13(uv,3.+13.*dep);'+
'c=mix(c,max(c,b),.55*amt);'+
'c=c*(1.-.10*amt)+.10*amt*vec3(.58,.63,.72)*lum(b);}'+
'else if(fx==24){float d=abs(uv.y-.5+(dep-.5)*.55);'+
'float f=smoothstep(.05,.32,d)*amt;'+
'c=mix(c,bl13(uv,1.+18.*f),f);c=sat(c,1.+.4*amt);'+
'c=(c-.5)*(1.+.18*amt)+.5;}'+
'else if(fx==25){float L=lum(c);vec3 d=vec3(L);'+
'vec3 ov=c*(c+2.*d*(1.-c));'+
'c=mix(c,clamp(ov,0.,1.),amt);c=sat(c,1.-.55*amt);'+
'c=(c-.5)*(1.+.55*amt)+.5;}'+
'else if(fx==26){float L=lum(c);'+
'vec3 g=mix(vec3(.05,.42,.62),vec3(1.,.62,.30),smoothstep(.12,.88,L));'+
'c=mix(c,c*g*1.55,.55*amt);c=sat(c,1.+.28*amt);'+
'c=(c-.5)*(1.+.2*amt)+.5;}'+
'else if(fx==27){float L=lum(c);'+
'c=mix(c,vec3(L),.55*amt);c*=vec3(.55,.72,1.28);'+
'c=pow(clamp(c,0.,1.),vec3(1.+1.15*amt));'+
'c+=smoothstep(.86,1.,L)*.5*amt;}'+
'else if(fx==28){c=vec3(pow(c.r,1.-.35*amt),pow(c.g,1.+.12*amt),pow(c.b,1.+.45*amt));'+
'c.r=c.r*1.06+.03*amt;c.b=c.b*.94+.06*amt;c=sat(c,1.+.35*amt);}'+
'else if(fx==30){vec3 b=bl13(uv,2.+7.*dep);float z=lum(b);'+
'z=pow(clamp(z,0.,1.),1.+(amt-.5)*1.6);c=vec3(z);}'+
'else if(fx==31){float e=1.+3.*dep;'+
'float hx=lu(vec2(e,0.))-lu(vec2(-e,0.));'+
'float hy=lu(vec2(0.,e))-lu(vec2(0.,-e));'+
'vec3 n=normalize(vec3(-hx*(4.+28.*amt),-hy*(4.+28.*amt),1.));'+
'c=n*.5+.5;}'+
'else if(fx==32){vec3 b=bl13(uv,3.);float z=1.-lum(b);'+
'vec3 fg=mix(cA,cB,.5+.5*sin(t*.18*spd));'+
'c=mix(c,fg,smoothstep(.12,1.,z)*amt);'+
'c+=fg*smoothstep(.6,1.,z)*.12*amt;}'+
'else if(fx==33){float a=t*.13*spd;'+
'vec2 lp=vec2(.5+.62*sin(a),.5+.45*cos(a*.73));'+
'float d=distance(uv*vec2(R.x/R.y,1.),lp*vec2(R.x/R.y,1.));'+
'float lk=exp(-d*d*(2.5+7.*(1.-dep)));'+
'vec3 lc=mix(cA,cB,.5+.5*sin(a*1.7));'+
'c=scr(c,lc*lk*amt*1.25);'+
'c=scr(c,lc*smoothstep(.34,1.,length(uv-.5))*.22*amt);}'+
'else if(fx==40){float L=lum(c);'+
'L=pow(clamp(L,0.,1.),1.+(dep-.5)*1.7);'+
'c=mix(c,mix(cA,cB,smoothstep(0.,1.,L)),amt);}'+
'else if(fx==41){vec3 th=vec3(.28+.42*dep);'+
'vec3 w2=smoothstep(th-.14,th+.14,c);'+
'c=mix(c,mix(c,1.-c,w2),amt);c=sat(c,1.+.3*amt);}'+
'else if(fx==42){vec2 p=uv-.5;p.x*=R.x/R.y;float r=length(p);'+
'float a=atan(p.y,p.x)+(1.-smoothstep(0.,.62,r))*2.6*amt*sin(t*.22*spd+r*7.*dep);'+
'vec2 q=vec2(cos(a),sin(a))*r;q.x/=R.x/R.y;'+
'c=texture2D(T,clamp(q+.5,.001,.999)).rgb;}'+
'else if(fx==43){vec2 p=uv-.5;p.x*=R.x/R.y;float r=length(p);'+
'float wv=sin(r*(18.+64.*dep)-t*2.3*spd)*.026*amt/(1.+r*2.6);'+
'vec2 q=uv+(p/max(r,1e-4))*wv;'+
'c=texture2D(T,clamp(q,.001,.999)).rgb;}'+
'else if(fx==44){vec3 zs=vec3(0.);'+
'for(int i=0;i<14;i++){float kk=1.-float(i)/14.*(.05+.24*amt)*(.4+dep);'+
'zs+=texture2D(T,.5+(uv-.5)*kk).rgb;}'+
'c=mix(c,zs/14.,.85);}'+
'else if(fx==45){vec2 pp=uv*2.-1.;pp*=1.+dot(pp,pp)*.08*dep;'+
'vec2 q=pp*.5+.5;'+
'if(q.x<0.||q.x>1.||q.y<0.||q.y>1.){c=vec3(0.);}else{'+
'vec3 sm=texture2D(T,q).rgb;'+
'float sl=.52+.48*sin(q.y*R.y*3.14159);'+
'float ms=.86+.14*sin(q.x*R.x*1.5708);'+
'sm*=mix(1.,sl*ms,amt);c=sm+vec3(.015,.025,.02)*amt;}}'+
'else if(fx==46){vec2 q=uv;float m=floor(dep*3.99);'+
'if(m<1.){q.x=abs(q.x-.5)+.5;}'+
'else if(m<2.){q.y=abs(q.y-.5)+.5;}'+
'else if(m<3.){q=abs(q-.5)+.5;}'+
'else{q=vec2(abs(q.x-.5)+.5,.5-abs(q.y-.5));}'+
'c=mix(c,texture2D(T,clamp(q,.001,.999)).rgb,amt);}'+
'else if(fx==47){float lv=mix(9.,3.,amt);'+
'vec3 q=floor(c*lv+.5)/lv;float sc=1.+2.5*dep;'+
'float gx=lu(vec2(-sc,0.))-lu(vec2(sc,0.));'+
'float gy=lu(vec2(0.,-sc))-lu(vec2(0.,sc));'+
'float e=smoothstep(.05,.30,length(vec2(gx,gy)));'+
'c=mix(q,cA*.2,e*.92);c=sat(c,1.18);}'+
'else if(fx==48){vec3 bs=vec3(0.);float bw=0.;'+
'for(int i=0;i<16;i++){float a=float(i)*.3927;'+
'float rr=(mod(float(i),4.)+1.)/4.;'+
'vec3 sm=texture2D(T,uv+vec2(cos(a),sin(a))*rr*(3.+24.*dep)/R).rgb;'+
'float kk=1.+7.*smoothstep(.58,1.,lum(sm));'+
'bs+=sm*kk;bw+=kk;}'+
'c=mix(c,bs/bw,amt);}'+
'else if(fx==49){float rows=mix(8.,44.,dep);'+
'float row=floor(uv.y*rows);'+
'float sd=h1(vec2(row,floor(t*(2.+9.*spd))));'+
'float on=step(1.-.38*amt,sd);'+
'float sh=(h1(vec2(row,7.7))-.5)*.3*amt*on;'+
'vec2 q=fract(uv+vec2(sh,0.));'+
'vec3 g=texture2D(T,q).rgb;'+
'g=mix(g,vec3(texture2D(T,fract(q+vec2(.014*amt,0.))).r,g.g,'+
'texture2D(T,fract(q-vec2(.014*amt,0.))).b),on);'+
'c=mix(c,g,max(on,.4));}'+
/* ── 50 DATAMOSH ─────────────────────────────────────────────────────────
   Not a glitch overlay: an actual motion estimate. One Lucas-Kanade step
   solves the brightness-constancy equation from the spatial gradient and the
   temporal delta, giving a per-pixel flow vector. The PREVIOUS output frame
   is then fetched along that vector, so moving regions drag their old pixels
   exactly the way a codec does when its P-frames lose their I-frame. The
   macroblock term quantises the sample point to an 8 px grid, which is where
   the characteristic blocky smear comes from. */
'if(fx==50){vec2 px=1./R;'+
'float gx=lum(texture2D(T,uv+vec2(px.x,0.)).rgb)-lum(texture2D(T,uv-vec2(px.x,0.)).rgb);'+
'float gy=lum(texture2D(T,uv+vec2(0.,px.y)).rgb)-lum(texture2D(T,uv-vec2(0.,px.y)).rgb);'+
'float dt=lum(c)-lum(texture2D(P,uv).rgb);'+
'vec2 g=vec2(gx,gy); float gg=dot(g,g)+1e-4;'+
'vec2 fl=clamp(-g*(dt/gg)*amt*6.,-0.07,0.07);'+
'vec3 sm=texture2D(P,uv+fl).rgb;'+
'vec2 bq=(floor(uv*R/8.)+0.5)*8./R;'+
'vec3 bk=texture2D(P,bq+fl).rgb;'+
'vec3 mo=mix(sm,bk,dep);'+
'gl_FragColor=vec4(mix(c,mo,clamp(length(fl)*22.+spd,0.,1.)),1.);return;}'+

/* ── 51 KUWAHARA ─────────────────────────────────────────────────────────
   Edge-preserving painterly abstraction. Four overlapping quadrants around
   each pixel; whichever has the lowest colour variance supplies the output.
   Flat areas smooth into slabs while edges stay razor sharp, which is why it
   reads as paint rather than blur. */
'if(fx==51){vec2 px=1./R; float rr=1.+amt*5.;'+
'vec3 mA=vec3(0.),mB=vec3(0.),mC=vec3(0.),mD=vec3(0.);'+
'vec3 sA=vec3(0.),sB=vec3(0.),sC=vec3(0.),sD=vec3(0.); float n=0.;'+
'for(int i=0;i<=4;i++){for(int j=0;j<=4;j++){'+
'  float fi=float(i)*rr*0.25, fj=float(j)*rr*0.25;'+
'  vec3 a=texture2D(T,uv+vec2(-fi,-fj)*px).rgb; mA+=a; sA+=a*a;'+
'  vec3 b=texture2D(T,uv+vec2( fi,-fj)*px).rgb; mB+=b; sB+=b*b;'+
'  vec3 d=texture2D(T,uv+vec2(-fi, fj)*px).rgb; mC+=d; sC+=d*d;'+
'  vec3 e=texture2D(T,uv+vec2( fi, fj)*px).rgb; mD+=e; sD+=e*e; n+=1.;}}'+
'mA/=n;mB/=n;mC/=n;mD/=n;sA/=n;sB/=n;sC/=n;sD/=n;'+
'float vA=dot(sA-mA*mA,vec3(1.)),vB=dot(sB-mB*mB,vec3(1.));'+
'float vC=dot(sC-mC*mC,vec3(1.)),vD=dot(sD-mD*mD,vec3(1.));'+
'vec3 o=mA; float mv=vA;'+
'if(vB<mv){o=mB;mv=vB;} if(vC<mv){o=mC;mv=vC;} if(vD<mv){o=mD;}'+
'gl_FragColor=vec4(mix(c,o,wet),1.);return;}'+

/* ── 52 FLOW STREAK ──────────────────────────────────────────────────────
   Smears ALONG the image structure rather than across it: the sample line is
   perpendicular to the local gradient, so strokes follow contours the way a
   brush follows a form. */
'if(fx==52){vec2 px=1./R;'+
'float gx=lum(texture2D(T,uv+vec2(px.x,0.)).rgb)-lum(texture2D(T,uv-vec2(px.x,0.)).rgb);'+
'float gy=lum(texture2D(T,uv+vec2(0.,px.y)).rgb)-lum(texture2D(T,uv-vec2(0.,px.y)).rgb);'+
'vec2 tg=normalize(vec2(-gy,gx)+1e-5);'+
'vec3 a=vec3(0.); float w=0.;'+
'for(int i=-8;i<=8;i++){float f=float(i);'+
'  float k=exp(-f*f/22.);'+
'  a+=texture2D(T,uv+tg*f*px*(2.+amt*14.)).rgb*k; w+=k;}'+
'gl_FragColor=vec4(mix(c,a/w,wet),1.);return;}'+

/* ── 53 PRISM SHATTER ────────────────────────────────────────────────────
   Breaks the frame into cells and refracts each one separately, with the
   three channels offset by different amounts so every shard splits light. */
'if(fx==53){float sc=6.+dep*26.;'+
'vec2 gid=floor(uv*sc); vec2 gf=fract(uv*sc);'+
'float rnd=h1(gid*7.13+1.7);'+
'float ang=rnd*6.2831+t*spd*0.35;'+
'vec2 nrm=vec2(cos(ang),sin(ang));'+
'float th=0.28+0.5*h1(gid*3.1);'+
'vec2 off=nrm*(th-0.5)*amt*0.14*(0.4+length(gf-0.5));'+
'float rr2=texture2D(T,uv+off*1.25).r;'+
'float gg2=texture2D(T,uv+off).g;'+
'float bb2=texture2D(T,uv+off*0.75).b;'+
'vec3 sh=vec3(rr2,gg2,bb2);'+
'float edge=smoothstep(0.,0.06,min(min(gf.x,gf.y),min(1.-gf.x,1.-gf.y)));'+
'sh+=(1.-edge)*grn*0.5;'+
'gl_FragColor=vec4(mix(c,sh,wet),1.);return;}'+

/* ── 54 TUNNEL ───────────────────────────────────────────────────────────
   Recursive feedback: the previous output, zoomed and rotated a hair each
   frame, so the image falls endlessly into itself. */
'if(fx==54){vec2 q=uv-0.5;'+
'float a2=(amt-0.5)*0.09; float zo=1.0-(dep-0.5)*0.06;'+
'mat2 rm=mat2(cos(a2),-sin(a2),sin(a2),cos(a2));'+
'vec2 pv=rm*q*zo+0.5;'+
'vec3 f=texture2D(P,pv).rgb;'+
'f*=0.90+0.10*sin(t*spd+length(q)*9.0);'+
'vec3 o=max(c*0.55,f*0.985);'+
'gl_FragColor=vec4(mix(c,o,wet),1.);return;}'+

/* ── 55 SPECTRAL BLOOM ───────────────────────────────────────────────────
   Highlights are isolated, spread on a wide ring, then tinted by angle so
   the halo disperses into wavelengths instead of glowing a flat white. */
'if(fx==55){vec3 b=vec3(0.); float w=0.;'+
'float rad=(2.+dep*22.);'+
'for(int i=0;i<24;i++){float f=float(i);'+
'  float a3=f*0.2618; vec2 d2=vec2(cos(a3),sin(a3));'+
'  float rr3=rad*(0.35+0.65*fract(f*0.618));'+
'  vec3 sm2=texture2D(T,uv+d2*rr3/R).rgb;'+
'  float hi=max(0.,lum(sm2)-(1.-amt)*0.75);'+
'  vec3 tint=0.5+0.5*cos(vec3(0.,2.094,4.188)+a3+t*spd*0.2);'+
'  b+=sm2*hi*tint; w+=1.;}'+
'b/=w; b*=3.2;'+
'gl_FragColor=vec4(mix(c,scr(c,b),wet),1.);return;}'+

/* ── 56 FLOW PAINT ───────────────────────────────────────────────────────
   Coherence-enhancing filtering, the technique behind painterly abstraction
   in film restoration. The structure tensor of the local gradient gives the
   dominant EDGE direction at every pixel; smearing ALONG that direction (and
   never across it) sharpens contours while dissolving flat areas into
   brushwork. Kuwahara flattens into facets; this one strokes. */
'if(fx==56){vec2 px=1./R;'+
'float gxx=0.,gyy=0.,gxy=0.;'+
'for(int i=-1;i<=1;i++){for(int j=-1;j<=1;j++){'+
'  vec2 o=vec2(float(i),float(j))*px;'+
'  float l1=lum(texture2D(T,uv+o+vec2(px.x,0.)).rgb);'+
'  float l2=lum(texture2D(T,uv+o-vec2(px.x,0.)).rgb);'+
'  float l3=lum(texture2D(T,uv+o+vec2(0.,px.y)).rgb);'+
'  float l4=lum(texture2D(T,uv+o-vec2(0.,px.y)).rgb);'+
'  float dx=l1-l2, dy=l3-l4;'+
'  gxx+=dx*dx; gyy+=dy*dy; gxy+=dx*dy;}}'+
/* minor eigenvector of the tensor = the direction of least change = the edge */
'float dd=sqrt(max(0.,(gxx-gyy)*(gxx-gyy)+4.*gxy*gxy));'+
'float l_1=(gxx+gyy+dd)*.5;'+
'vec2 dir=normalize(vec2(gxy,l_1-gxx)+vec2(1e-5));'+
'float coh=dd/(gxx+gyy+1e-4);'+          /* how directional this pixel is */
'float len=(2.+dep*22.)*mix(.35,1.,coh);'+
'vec3 acc=vec3(0.);float wsum=0.;'+
'for(int k2=-8;k2<=8;k2++){float fk=float(k2)/8.;'+
'  float wk=exp(-fk*fk*2.2);'+
'  acc+=texture2D(T,uv+dir*fk*len*px).rgb*wk; wsum+=wk;}'+
'vec3 pn2=acc/wsum;'+
'pn2=sat(pn2,1.+.5*amt);'+
'gl_FragColor=vec4(mix(c,pn2,wet*amt),1.);return;}'+

/* ── 57 CAUSTIC GLASS ────────────────────────────────────────────────────
   The image seen through moving water. An animated fbm height field is
   differentiated into a surface normal, and each channel is refracted with
   its own index -- which is exactly why real glass fringes colour. The
   caustic highlight is the divergence of the normal field: where light
   converges, it is bright. */
'if(fx==57){float sc2=3.+dep*14.;float tt=t*spd*.35;'+
'vec2 wq=uv*sc2+vec2(tt,tt*.7);'+
'float e2=.02;'+
'float h0=fbm(wq);'+
'float hx=fbm(wq+vec2(e2,0.))-h0;'+
'float hy=fbm(wq+vec2(0.,e2))-h0;'+
'vec2 nrm=vec2(hx,hy)/e2;'+
'float str=amt*.055;'+
'vec3 g2;'+
'g2.r=texture2D(T,uv-nrm*str*1.00).r;'+
'g2.g=texture2D(T,uv-nrm*str*1.06).g;'+
'g2.b=texture2D(T,uv-nrm*str*1.13).b;'+
'float div=(fbm(wq+vec2(e2,0.))+fbm(wq-vec2(e2,0.))'+
'          +fbm(wq+vec2(0.,e2))+fbm(wq-vec2(0.,e2))-4.*h0)/(e2*e2);'+
'float cau=smoothstep(0.,90.,-div)*amt;'+
'g2+=cau*.55*mix(vec3(1.),cA,.5);'+
'gl_FragColor=vec4(mix(c,g2,wet),1.);return;}'+

/* ── 58 RELIEF ───────────────────────────────────────────────────────────
   Parallax occlusion mapping with luminance standing in for depth: the view
   ray is marched through a heightfield until it goes under the surface, so
   bright things genuinely sit in front and occlude what is behind them as
   the angle moves. This is the trick that gave 2000s game walls their
   bricks, applied to a live camera. */
'if(fx==58){float amp=dep*.09;'+
'vec2 ray=vec2(cos(t*spd*.5),sin(t*spd*.37))*amp*(0.35+amt);'+
'vec2 pp2=uv;float hh=1.;vec2 stp=ray/12.;'+
'for(int i2=0;i2<12;i2++){'+
'  float hm=1.-lum(texture2D(T,pp2).rgb);'+
'  if(hm<hh){break;}'+
'  pp2-=stp; hh-=1./12.;}'+
'vec3 r2=texture2D(T,pp2).rgb;'+
/* shade the slope so the relief actually reads as geometry */
'float sx=lum(texture2D(T,pp2+vec2(2./R.x,0.)).rgb)'+
'        -lum(texture2D(T,pp2-vec2(2./R.x,0.)).rgb);'+
'float sy=lum(texture2D(T,pp2+vec2(0.,2./R.y)).rgb)'+
'        -lum(texture2D(T,pp2-vec2(0.,2./R.y)).rgb);'+
'vec3 nn=normalize(vec3(-sx*6.,-sy*6.,1.));'+
'float dif=max(0.,dot(nn,normalize(vec3(.5,.7,.6))));'+
'r2*=(.55+.75*dif);'+
'gl_FragColor=vec4(mix(c,r2,wet),1.);return;}'+

/* ── 59 ROSETTE ──────────────────────────────────────────────────────────
   A real CMYK halftone, not a dot pattern. Each ink gets its own screen
   angle -- 15, 75, 0, 45 degrees -- because those are the angles the print
   trade settled on to keep the dots from colliding into moire. The
   interference of the four screens is the rosette you see in a magazine
   under a loupe. */
/* Written out four times rather than looped over a vec4. Indexing a vector
   with a loop variable AS AN L-VALUE is the kind of GLSL that compiles on a
   desktop driver and silently fails on a phone -- and phones are what this
   camera is looked at through. */
'if(fx==59){float fr=mix(45.,190.,1.-dep);'+
'vec2 ar=uv*R/R.y;'+
'vec3 cmy=1.-c; float kk2=min(cmy.r,min(cmy.g,cmy.b))*amt;'+
'cmy=(cmy-kk2)/max(1e-3,1.-kk2);'+
'float dC=scrn(ar,fr,.2618,cmy.r);'+       /* cyan     15 deg */
'float dM=scrn(ar,fr,1.3090,cmy.g);'+      /* magenta  75 deg */
'float dY=scrn(ar,fr,0.,cmy.b);'+          /* yellow    0 deg */
'float dK=scrn(ar,fr,.7854,kk2);'+         /* black    45 deg */
'vec3 out2=vec3(1.);'+
'out2-=dC*vec3(0.,.78,.78);'+              /* cyan absorbs red    */
'out2-=dM*vec3(.86,0.,.60);'+              /* magenta absorbs green */
'out2-=dY*vec3(.10,.10,.92);'+             /* yellow absorbs blue */
'out2-=dK*vec3(.88);'+
'gl_FragColor=vec4(mix(c,clamp(out2,0.,1.),wet),1.);return;}'+

/* ── 60 CHROMA ECHO ──────────────────────────────────────────────────────
   Each colour channel decays at its own rate in the feedback buffer, so a
   moving subject leaves red, green and blue trailing at different lengths
   and separates into a spectrum behind itself. One buffer, three time
   constants -- the whole effect is that the channels disagree about how long
   ago now was. */
'if(fx==60){vec2 dr=(uv-.5)*(1.-dep*.012)+.5;'+
'vec3 pv2=texture2D(P,dr).rgb;'+
'vec3 dk=vec3(.86+.13*amt,.79+.18*amt,.70+.24*amt);'+
'vec3 tr=pv2*dk;'+
'vec3 o3=max(c,tr);'+
'o3=sat(o3,1.15);'+
'gl_FragColor=vec4(mix(c,o3,wet),1.);return;}'+

/* ── tracked-object boxes ───────────────────────────────────────────────
   bmode 1 = glitch only INSIDE a box, 2 = only outside, 3 = draw the frame.
   Boxes come from the mask's connected blobs, so they are the real extents
   of a moving thing rather than a guess. */
'if(bmode>0){'+
'float ins=0.,edge=0.;'+
'for(int i=0;i<8;i++){ if(i>=nbx) break;'+
'  vec4 B=bx[i];'+
'  if(uv.x>B.x&&uv.x<B.z&&uv.y>B.y&&uv.y<B.w){ ins=1.;'+
'    float ex=min(uv.x-B.x,B.z-uv.x), ey=min(uv.y-B.y,B.w-uv.y);'+
'    float e=min(ex*R.x,ey*R.y);'+
'    if(e<1.6) edge=1.; }'+
'}'+
'if(bmode==3){ c=mix(c,cA,edge); }'+
'else { float g=(bmode==1)?ins:1.-ins; c=mix(orig,c,g); }'+
'}'+
'if(mmode>0){'+
'float mv=texture2D(M,uv).r;'+
'float gt;'+
'if(mmode==1){gt=mv;}'+                                  /* subject only     */
'else if(mmode==2){gt=1.-mv;}'+                          /* background only  */
'else{'+                                                 /* outline only     */
'float e=abs(mv-texture2D(M,uv+vec2(2.,0.)/R).r)'+
'+abs(mv-texture2D(M,uv+vec2(0.,2.)/R).r);'+
'gt=clamp(e*7.,0.,1.);}'+
'c=mix(orig,c,gt);}'+
'c=mix(orig,c,wet);'+
'gl_FragColor=vec4(clamp(c,0.,1.),1.);}';

/* Point-cloud program: real 3D reconstruction. The vertex shader samples the
   camera texture (vertex texture fetch), turns luminance into Z, rotates the
   cloud in 3D and projects it — the same idea as a TD TOP-to-instance rig. */
var PCV=
'precision highp float;attribute vec2 ap;uniform sampler2D T;'+
'uniform float t,amt,dep,spd,psz;uniform vec2 R;varying vec3 vc;varying float vz;'+
'void main(){vec3 col=texture2D(T,ap).rgb;vc=col;'+
'float l=dot(col,vec3(.299,.587,.114));'+
'vec3 p=vec3((ap-.5)*2.,(l-.5)*(.5+2.5*dep));'+
'p.x*=R.x/R.y;'+
'float a=t*.45*spd;float ca=cos(a),sa=sin(a);'+
'p=vec3(p.x*ca+p.z*sa,p.y,-p.x*sa+p.z*ca);'+
'float b=sin(t*.29*spd)*.45*amt;float cb=cos(b),sb=sin(b);'+
'p=vec3(p.x,p.y*cb-p.z*sb,p.y*sb+p.z*cb);'+
'float zc=p.z+3.4;vz=zc;'+
'vec2 sp=p.xy*(2.6/zc);sp.x/=R.x/R.y;'+
'gl_Position=vec4(sp,0.,1.);'+
'gl_PointSize=clamp(psz*(3.4/zc),1.,14.);}';
var PCF=
'precision highp float;varying vec3 vc;varying float vz;'+
'void main(){vec2 d=gl_PointCoord-.5;float r=dot(d,d);'+
'if(r>.25)discard;'+
'float al=1.-smoothstep(.05,.25,r);'+
'float fog=clamp((5.2-vz)*.55,.15,1.);'+
'gl_FragColor=vec4(vc*fog,al);}';

function mkTex(w,h){
  var x=gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D,x);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR);
  gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR);
  if(w)gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,w,h,0,gl.RGBA,gl.UNSIGNED_BYTE,null);
  return x;
}
function glInit(){
  if(!glWatchOnce){ glWatchOnce=true; glWatch(gc); }
  try{
    var o={preserveDrawingBuffer:true,antialias:false,alpha:false};
    gl=gc.getContext('webgl',o)||gc.getContext('experimental-webgl',o);
  }catch(e){gl=null}
  if(!gl){glDie('no webgl',true);return}
  function sh(ty,src){
    var x=gl.createShader(ty);
    gl.shaderSource(x,src);gl.compileShader(x);
    if(!gl.getShaderParameter(x,gl.COMPILE_STATUS)){
      toast('FX shader: '+(gl.getShaderInfoLog(x)||'').slice(0,70),true);return null}
    return x;
  }
  function link(vs,fs){
    var pr=gl.createProgram();
    gl.attachShader(pr,vs);gl.attachShader(pr,fs);gl.linkProgram(pr);
    return gl.getProgramParameter(pr,gl.LINK_STATUS)?pr:null;
  }
  // ── fullscreen-quad FX program ──
  var v1=sh(gl.VERTEX_SHADER,
    'attribute vec2 a;varying vec2 v;void main(){v=a;gl_Position=vec4(a*2.-1.,0.,1.);}');
  var f1=sh(gl.FRAGMENT_SHADER,FSRC);
  if(!v1||!f1||!(glP=link(v1,f1))){glDie('shader/link',true);return}
  quadBuf=gl.createBuffer();
  gl.bindBuffer(gl.ARRAY_BUFFER,quadBuf);
  gl.bufferData(gl.ARRAY_BUFFER,new Float32Array([0,0,1,0,0,1,1,1]),gl.STATIC_DRAW);
  gl.useProgram(glP);
  ['T','P','t','fx','amt','dep','spd','grn','k','R','cA','cB','wet','zm','pn','M','mmode','bx','nbx','bmode'].forEach(function(n){
    glU[n]=gl.getUniformLocation(glP,n)});
  gl.uniform1i(glU.T,0);gl.uniform1i(glU.P,1);gl.uniform1i(glU.M,2);
  glTex=mkTex();

  // ── point-cloud program (needs vertex texture fetch) ──
  if(gl.getParameter(gl.MAX_VERTEX_TEXTURE_IMAGE_UNITS)>0){
    var v2=sh(gl.VERTEX_SHADER,PCV), f2=sh(gl.FRAGMENT_SHADER,PCF);
    if(v2&&f2&&(pcP=link(v2,f2))){
      gl.useProgram(pcP);
      ['T','t','amt','dep','spd','psz','R'].forEach(function(n){
        pcU[n]=gl.getUniformLocation(pcP,n)});
      gl.uniform1i(pcU.T,0);
      var GW=256,GH=192,arr=new Float32Array(GW*GH*2),k=0;
      for(var y=0;y<GH;y++)for(var x=0;x<GW;x++){
        arr[k++]=(x+.5)/GW; arr[k++]=(y+.5)/GH; }
      pcN=GW*GH;
      pcBuf=gl.createBuffer();
      gl.bindBuffer(gl.ARRAY_BUFFER,pcBuf);
      gl.bufferData(gl.ARRAY_BUFFER,arr,gl.STATIC_DRAW);
    }
  }
  rlog('GLINIT quad='+(!!glP)+' pc='+(!!pcP)+
       ' vtf='+gl.getParameter(gl.MAX_VERTEX_TEXTURE_IMAGE_UNITS)+
       ' mr='+(!!window.MediaRecorder)+
       ' share='+(!!(navigator.share&&navigator.canShare)));
  // NOTE: the <img> stays visible until render() proves a frame drew.
  // Hiding it here is what produced a blank screen when GL failed later.
}
function glSize(w,h){
  gc.width=w;gc.height=h;
  for(var i=0;i<2;i++){
    if(fbt[i])gl.deleteTexture(fbt[i]);
    if(fbo[i])gl.deleteFramebuffer(fbo[i]);
    fbt[i]=mkTex(w,h);
    fbo[i]=gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER,fbo[i]);
    gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,fbt[i],0);
    gl.clearColor(0,0,0,1);gl.clear(gl.COLOR_BUFFER_BIT);
    if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE){
      glDie('framebuffer '+w+'x'+h);return;}
  }
  gl.bindFramebuffer(gl.FRAMEBUFFER,null);
  if(accOn) accAlloc();            // keep the exposure buffers in step
  if(mTex)  maskAlloc();           // and the segmentation buffers
  if(bTex)  boxAlloc();            // and the box-readback buffer
}
function maskAlloc(){
  for(var j=0;j<2;j++){
    if(bgTex[j])gl.deleteTexture(bgTex[j]);
    if(bgFbo[j])gl.deleteFramebuffer(bgFbo[j]);
    bgTex[j]=mkTex(gc.width,gc.height);
    bgFbo[j]=gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER,bgFbo[j]);
    gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,bgTex[j],0);
    gl.clearColor(0,0,0,1); gl.clear(gl.COLOR_BUFFER_BIT);
  }
  var oldM=mTex;
  if(mTex)gl.deleteTexture(mTex);
  if(mFbo)gl.deleteFramebuffer(mFbo);
  mTex=mkTex(gc.width,gc.height);
  mFbo=gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER,mFbo);
  gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,mTex,0);
  gl.bindFramebuffer(gl.FRAMEBUFFER,null);
  // maskTex still pointed at the texture we just deleted. Binding a deleted
  // texture makes EVERY drawArrays fail with INVALID_OPERATION -- the whole
  // preview goes black, not just the mask -- so repoint it at the new one.
  if(maskTex===oldM) maskTex=mTex;
  bgReady=0;
}
function accAlloc(){                 // only when the exposure engine is armed
  for(var j=0;j<2;j++){
    if(accT[j])gl.deleteTexture(accT[j]);
    if(accFbo[j])gl.deleteFramebuffer(accFbo[j]);
    accT[j]=mkTex(gc.width,gc.height);
    accFbo[j]=gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER,accFbo[j]);
    gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,accT[j],0);
  }
  gl.bindFramebuffer(gl.FRAMEBUFFER,null);
}
function nowT(){return ((Date.now()-glT0)%100000)/1000;}
function glPass(fb,fxv,tex,prev,vw,vh){
  gl.useProgram(glP);
  gl.bindBuffer(gl.ARRAY_BUFFER,quadBuf);
  var a=gl.getAttribLocation(glP,'a');
  gl.enableVertexAttribArray(a);
  gl.vertexAttribPointer(a,2,gl.FLOAT,false,0,0);
  gl.bindFramebuffer(gl.FRAMEBUFFER,fb);
  gl.viewport(0,0,vw||gc.width,vh||gc.height);
  gl.disable(gl.BLEND);
  gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,tex);
  gl.activeTexture(gl.TEXTURE1);gl.bindTexture(gl.TEXTURE_2D,prev||fbt[1-pp]||tex);
  gl.uniform1i(glU.fx,fxv);
  gl.uniform1f(glU.amt,fxAmt);
  gl.uniform1f(glU.dep,fxDepth);
  gl.uniform1f(glU.spd,fxSpeed);
  gl.uniform1f(glU.grn,fxGrain);
  gl.uniform1f(glU.k,accK);
  gl.uniform3f(glU.cA,fxCA[0],fxCA[1],fxCA[2]);
  gl.uniform3f(glU.cB,fxCB[0],fxCB[1],fxCB[2]);
  gl.uniform1f(glU.wet,(fxv===98||fxv===99||fxv>=90)?1:fxWet);
  // the copy/grade passes read a framebuffer that is already zoomed
  var zoomed=(fxv===98||fxv===99||fxv>=90);
  gl.uniform1f(glU.zm, zoomed?1:fxZoom);
  gl.uniform2f(glU.pn, zoomed?0:fxPanX, zoomed?0:fxPanY);
  // Never sample the mask while rendering INTO it: a texture bound as both
  // input and colour attachment is a feedback loop, and WebGL answers by
  // failing the whole draw with INVALID_OPERATION -- silently, every frame.
  var mt = (fb && fb===mFbo) ? null : maskTex;
  gl.activeTexture(gl.TEXTURE2);
  gl.bindTexture(gl.TEXTURE_2D, mt || tex);
  gl.uniform1i(glU.mmode, (zoomed||!mt) ? 0 : maskMode);
  // helper passes (>=90) must never see the boxes, or the mask would be
  // gated by the very boxes it is about to produce
  var bm = (zoomed || fxv >= 90) ? 0 : boxMode;
  gl.uniform1i(glU.bmode, bm);
  gl.uniform1i(glU.nbx, bm ? Math.min(boxes.length, 8) : 0);
  if (bm && boxes.length) {
    var fl = new Float32Array(32);
    for (var bi = 0; bi < Math.min(boxes.length, 8); bi++) {
      var B = boxes[bi];
      fl[bi*4] = B.x0; fl[bi*4+1] = B.y0; fl[bi*4+2] = B.x1; fl[bi*4+3] = B.y1;
    }
    gl.uniform4fv(glU.bx, fl);
  }
  gl.uniform1f(glU.t,nowT());
  gl.uniform2f(glU.R,gc.width,gc.height);
  gl.drawArrays(gl.TRIANGLE_STRIP,0,4);
}
function pcPass(fb,srcTex){
  gl.useProgram(pcP);
  gl.bindBuffer(gl.ARRAY_BUFFER,pcBuf);
  var a=gl.getAttribLocation(pcP,'ap');
  gl.enableVertexAttribArray(a);
  gl.vertexAttribPointer(a,2,gl.FLOAT,false,0,0);
  gl.bindFramebuffer(gl.FRAMEBUFFER,fb);
  gl.viewport(0,0,gc.width,gc.height);
  gl.clearColor(0.015,0.02,0.035,1);gl.clear(gl.COLOR_BUFFER_BIT);
  gl.enable(gl.BLEND);
  gl.blendFunc(gl.SRC_ALPHA,gl.ONE_MINUS_SRC_ALPHA);
  gl.activeTexture(gl.TEXTURE0);gl.bindTexture(gl.TEXTURE_2D,srcTex||glTex);
  gl.uniform1f(pcU.t,nowT());
  gl.uniform1f(pcU.amt,fxAmt);
  gl.uniform1f(pcU.dep,fxDepth);
  gl.uniform1f(pcU.spd,fxSpeed);
  gl.uniform1f(pcU.psz,1.5+7.*fxDepth);
  gl.uniform2f(pcU.R,gc.width,gc.height);
  gl.drawArrays(gl.POINTS,0,pcN);
  gl.disable(gl.BLEND);
}
function render(){
  if(!gl||glLost||!img.naturalWidth)return;
  try{
    /* The pipeline used to run at exactly the source size, so everything
       PROCEDURAL -- grain, contour lines, glitch blocks, slit-scan columns --
       quantised to the camera's own pixel grid and looked coarse. Rendering
       above the source does not invent detail in the photograph, but it does
       let the generated detail resolve properly, which is what actually reads
       as low resolution. */
    var _w=Math.round(img.naturalWidth*fxScale), _h=Math.round(img.naturalHeight*fxScale);
    if(_w>2048){ var _k=2048/_w; _w=2048; _h=Math.round(_h*_k); }   // GL ceiling
    if(gc.width!==_w||gc.height!==_h){
      glSize(_w,_h);
      if(!gl)return;                       // glSize may have bailed
    }
    if(texDirty){                          // upload only when a new frame arrived
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,true);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D,glTex);
      gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,img);
      texDirty=false;
      texCount++;
    }
    if(texCount===0)return;                // nothing uploaded yet — keep showing <img>
    if(viewRotStale()) applyViewRot();
    if(maskMode>0 && maskSrc==='motion'){
      if(!mTex) maskAlloc();
      var savedK=accK, savedA=fxAmt;
      // first frames snap the background in hard, then it drifts slowly so
      // a subject that stops moving does not dissolve into the scene
      accK = (bgReady<30) ? 0.35 : bgRate;
      glPass(bgFbo[bgPP], 94, glTex, bgTex[1-bgPP]);
      bgPP = 1-bgPP; bgReady++;
      fxAmt = maskThresh;
      glPass(mFbo, 95, glTex, bgTex[1-bgPP]);
      maskTex = mTex;
      // every other frame: readPixels stalls the pipeline, and objects do not
      // move far enough in 33 ms to need it any more often than that
      if (boxMode > 0 && (texCount & 1) === 0) boxDetect();
      accK = savedK; fxAmt = savedA;
    }
    if(fxN===11&&pcP) pcPass(fbo[pp]);     // 3D point cloud → feedback buffer
    else              glPass(fbo[pp],fxN,glTex);

    if(accOn&&!accFrozen){                 // composite this frame into the exposure
      accN++;
      accK=(accMode===91)?1/accN
          :(accMode===93)?Math.min(1,(Date.now()-accStart)/accDur)
          :0;
      glPass(accFbo[accPP],accMode,fbt[pp],accT[1-accPP]);
      accPP=1-accPP;
    }
    glPass(null,finalFx(),(accOn?accT[1-accPP]:fbt[pp]));   // plain copy unless an effect, exposure or grain was asked for
    pp=1-pp;
    if(!glReady){                          // first proven frame — now take over
      glReady=true;
      gc.classList.add('on');
      img.classList.add('dim');
      rlog('GLREADY '+gc.width+'x'+gc.height);
    }
  }catch(e){ glDie(((e&&e.name)||'err')+': '+((e&&e.message)||'').slice(0,90)); }
}
/* Animation loop decoupled from stream fps: effects evolve at display rate
   (60 fps) while the camera texture refreshes whenever a frame lands. This is
   what makes rotation, feedback and noise read as fluid instead of steppy. */
var rafBeat=0;
function drawTick(){
  render();
  if(dCtx&&img.naturalWidth) drawRec();
}
function loop(){
  rafBeat=Date.now();
  drawTick();
  requestAnimationFrame(loop);
}
/* Backstop: if rAF has not ticked for 500 ms (hidden tab, throttling, iOS low
   power) drive the pipeline from a timer instead, so recordings keep receiving
   frames and the watchdog never mistakes throttling for failure. */
setInterval(function(){
  if(document.hidden)return;
  if(Date.now()-rafBeat>500) drawTick();
},250);
pasarBoot();
/* Sequential JPEG polling: exactly one request in flight at any time.
   Each frame is a COMPLETE http response, so iOS Safari settles the page
   (an MJPEG stream never completes and leaves the spinner running forever).
   A watchdog re-arms the loop if a request stalls, so a dropped packet can
   never wedge the viewer. */
function nextFrame(){
  if(!run||busy)return;
  busy=true;
  clearTimeout(wd);
  wd=setTimeout(function(){ busy=false; nextFrame(); },4000);
  reqT=(window.performance&&performance.now)?performance.now():Date.now();
  img.src='/jpg?'+(Date.now());
}
img.onload=function(){
  busy=false; clearTimeout(wd);
  se.classList.remove('on');
  texDirty=true;              // rAF loop picks this up and re-uploads
  if(viewRotStale()) applyViewRot();
  if(!gotFirst){gotFirst=true;rlog('FRAME1 '+img.naturalWidth+'x'+img.naturalHeight);}
  if(reqT){
    var nw=(window.performance&&performance.now)?performance.now():Date.now();
    latAcc+=(nw-reqT); latN++; reqT=0;
  }
  if(++nf>=10){
    var f=(nf*1000/(Date.now()-t0)).toFixed(1);
    latShown=latN?Math.round(latAcc/latN):0;
    document.getElementById('fps').textContent=f+' fps  \u00b7  '+latShown+' ms'+
      (autoRate&&nHere>1?('  \u00b7  shared x'+nHere):'');
    latAcc=0; latN=0; nf=0; t0=Date.now();
  }
  if(run) tmr=setTimeout(nextFrame,previewGap());
};
img.onerror=function(){
  busy=false; clearTimeout(wd);
  se.classList.add('on');
  if(run) tmr=setTimeout(nextFrame,1200);
};
var autoRate=true, nHere=1, recBusy=false;
/* While the camera is recording, the live preview yields: every preview frame sent over WiFi is
   work the recorder could use, so it runs at about 8 fps until the clip is done. */
function previewGap(){ return (recOn||recBusy)?Math.max(gap,125):gap; }
function setRate(v){
  autoRate=(String(v)==='0');
  gap=parseInt(v,10)||0;
  applyRate();
}
function applyRate(){
  if(!autoRate)return;
  // one viewer: go flat out. more: leave the server room to answer control
  // requests, otherwise the shared session starves behind frame traffic.
  gap = nHere<=1 ? 0 : (nHere===2 ? 45 : (nHere===3 ? 80 : 120));
}
function togglePause(){
  run=!run;
  document.getElementById('btn-pause').classList.toggle('on',!run);
  if(run){ busy=false; nextFrame(); toast('Stream resumed'); }
  else { clearTimeout(tmr); clearTimeout(wd); toast('Stream paused'); }
}
document.addEventListener('visibilitychange',function(){
  if(document.hidden){ clearTimeout(tmr); clearTimeout(wd); busy=false; }
  else {
    if(run){ busy=false; nextFrame(); }
    rafBeat=Date.now();
    if(!gl&&!glFatal){          // effects were shut off by throttling — rebuild
      glReady=false; texCount=0;
      try{ glInit(); rlog('GL reinit on resume'); }catch(e){}
    }
    glCheckLost();              // iOS may have taken the context while we were away
  }
});
window.addEventListener('pageshow',function(e){ if(e.persisted) glCheckLost(); });
(function(){   // flexbox `gap` — Safari <14.1 ignores it and rows collapse together
  try{
    var d=document.createElement('div');
    d.style.cssText='display:flex;flex-direction:column;row-gap:8px;position:absolute;visibility:hidden';
    d.innerHTML='<i></i><i></i>';
    document.body.appendChild(d);
    var ok=d.scrollHeight>=8;
    document.body.removeChild(d);
    if(!ok){ document.documentElement.className+=' nogap'; rlog('NOFLEXGAP'); }
  }catch(e){}
})();
(function(){                       // <input type=color> is missing on some
  var t=document.createElement('input');            // older Android/Linux builds
  try{t.type='color'}catch(e){}
  if(t.type!=='color'){
    document.getElementById('colrow').style.display='none';
    document.getElementById('huerow').style.display='';
    rlog('NOCOLORINPUT');
  }
})();
fxLabels();
glInit();

/* Pinch to zoom, drag to pan, double-tap to reset — on the viewfinder itself,
   the way a camera should behave. Zoom is a texture crop, so it costs nothing
   and it applies to FX stills and recordings as well as the live view. */
(function(){
  var sv=document.querySelector('.sv');
  if(!sv)return;
  var d0=0, z0=1, lx=0, ly=0, dragging=false, lastTap=0;
  var pressAt=0, originX=0, originY=0, gesture='', startLevel=0, panLocked=false;
  var tapPossible=false, multitouch=false;
  function dist(t){var a=t[0],b=t[1];
    return Math.sqrt(Math.pow(a.clientX-b.clientX,2)+Math.pow(a.clientY-b.clientY,2));}
  function clampPan(){
    var m=Math.max(0,(1-1/fxZoom)*0.5);      // never pan past the frame edge
    fxPanX=Math.max(-m,Math.min(m,fxPanX));
    fxPanY=Math.max(-m,Math.min(m,fxPanY));
  }
  function showZoom(){
    var el=document.getElementById('zval');
    if(el)el.textContent=fxZoom.toFixed(1)+'x';
    var sl=document.getElementById('sl-zoom');
    if(sl)sl.value=Math.round(fxZoom*100);
    applyViewRot();
  }
  sv.addEventListener('touchstart',function(e){
    if(e.touches.length===2){
      d0=dist(e.touches); z0=fxZoom; dragging=false; gesture='';
      panLocked=false; tapPossible=false; multitouch=true;
    }
    else if(e.touches.length===1){
      dragging=true; lx=e.touches[0].clientX; ly=e.touches[0].clientY;
      originX=lx; originY=ly; pressAt=Date.now(); gesture=''; panLocked=false;
      tapPossible=!multitouch;
    }
  },{passive:true});
  sv.addEventListener('touchmove',function(e){
    if(e.cancelable)e.preventDefault();
    if(e.touches.length===2 && d0>0){
      fxZoom=Math.max(1,Math.min(4, z0*dist(e.touches)/d0));
      clampPan(); showZoom();
    } else if(e.touches.length===1){
      var t=e.touches[0];
      var dx=t.clientX-originX, dy=t.clientY-originY;
      if(Math.abs(dx)>10 || Math.abs(dy)>10) tapPossible=false;
      if(!gesture && !panLocked && Date.now()-pressAt>220 && Math.abs(dy)>12 &&
         Math.abs(dy)>Math.abs(dx)*1.3){
        gesture=originX<sv.getBoundingClientRect().left+sv.clientWidth/2
          ? 'ae' : 'contrast';
        var start=document.getElementById(gesture==='ae'?'sl-ae':'sl-contrast');
        startLevel=start?parseInt(start.value,10):0;
        dragging=false;
      }
      if(gesture){
        var input=document.getElementById(gesture==='ae'?'sl-ae':'sl-contrast');
        if(input){
          var value=Math.max(-2,Math.min(2,startLevel-Math.round(dy/45)));
          if(value!==parseInt(input.value,10)){
            input.value=value;
            var output=document.getElementById(gesture==='ae'?'vael':'vco');
            if(output)output.textContent=value;
            api('/set?'+(gesture==='ae'?'ae_level':'contrast')+'='+value);
          }
        }
      } else if(dragging && fxZoom>1){
        if(Math.abs(dx)>8 || Math.abs(dy)>8) panLocked=true;
        /* a drag on the screen, turned back into a drag across the picture */
        var sx=t.clientX-lx, sy=t.clientY-ly, du, dvv;
        switch(viewTurn()){
          case 1: du=sy;  dvv=-sx; break;
          case 2: du=-sx; dvv=-sy; break;
          case 3: du=-sy; dvv=sx;  break;
          default: du=sx; dvv=sy;
        }
        var quarter=viewTurn()&1;
        fxPanX -= du/(quarter?sv.clientHeight:sv.clientWidth)/fxZoom;
        fxPanY += dvv/(quarter?sv.clientWidth:sv.clientHeight)/fxZoom;
        lx=t.clientX; ly=t.clientY; clampPan();
      }
    }
  },{passive:false});
  sv.addEventListener('touchend',function(e){
    if(e.touches.length===0){
      var now=Date.now();
      if(tapPossible && !gesture && now-pressAt<250){
        if(lastTap && now-lastTap<300){
          fxZoom=1; fxPanX=fxPanY=0; showZoom(); toast('Zoom reset');
          lastTap=0;
        } else lastTap=now;
      }
      d0=0; dragging=false; gesture=''; panLocked=false;
      tapPossible=false; multitouch=false;
    }
  },{passive:true});
  // desktop: wheel zooms
  sv.addEventListener('wheel',function(e){
    e.preventDefault();
    fxZoom=Math.max(1,Math.min(4, fxZoom*(e.deltaY<0?1.1:0.9)));
    clampPan(); showZoom();
  },{passive:false});
  window.setZoom=function(v){ fxZoom=parseInt(v,10)/100; clampPan(); showZoom(); };
})();
// If WebGL never renders a usable frame in 4s, drop back to the raw stream
// so the viewer is never left staring at a blank canvas.
function glWatchdog(){
  if(document.hidden){ setTimeout(glWatchdog,4000); return; }   // not a failure
  if(!gotFirst){ rlog('NOSTREAM after 8s'); toast('No stream from camera',true);
                 setTimeout(glWatchdog,8000); return; }
  if(gl&&!glReady) glDie('no GL frame in 8s');
}
setTimeout(glWatchdog,8000);
/* Desktop keyboard control — ignored while typing in a field. */
document.addEventListener('keydown',function(e){
  var t=e.target.tagName;
  if(t==='INPUT'||t==='SELECT'||t==='TEXTAREA')return;
  var k=e.key.toLowerCase();
  if(k===' '){e.preventDefault();fxPhoto();}
  else if(k==='v'){toggleDevRec();}
  else if(k==='e'){toggleExp();}
  else if(k==='p'){togglePause();}
  else if(k==='j'){saveJpg();}
  else if(k==='n'){savePng();}
  else if(k==='m'){toggleMotion();}
  else if(k==='s'){fireShot();}
  else if(k==='k'){
    var o=document.getElementById('sel-skin');
    o.selectedIndex=(o.selectedIndex+1)%o.options.length;
    setSkin(o.value);
  }
  else if(k==='['||k===']'){
    var sel=document.getElementById('sel-fx'),o=sel.options,i=sel.selectedIndex;
    i=(k===']')?(i+1)%o.length:(i-1+o.length)%o.length;
    sel.selectedIndex=i;fxN=parseInt(o[i].value,10);fxLabels();
    toast(o[i].text);
  }
},false);
// Re-fit after rotation / window resize so the canvas never ends up stale.
window.addEventListener('resize',function(){ if(gl) gc.width=1; },false);
window.addEventListener('orientationchange',function(){ if(gl) gc.width=1; },false);
requestAnimationFrame(loop);
nextFrame();

function api(u){
  return fetch(u).then(function(r){
    return r.text().then(function(txt){
      try{ return JSON.parse(txt); }
      catch(e){
        // A non-JSON reply almost always means the request was answered by the
        // wrong handler. Report it rather than quietly returning {}.
        rlog('BADJSON '+u+' -> '+r.status+' '+txt.slice(0,40));
        return {};
      }
    });
  }).catch(function(){ rlog('NETFAIL '+u); return {}; });
}
function sv(id,v){document.getElementById(id).textContent=v}
var tT;
function phase(t){var p=document.getElementById('phase');if(p)p.textContent=t;}
function toast(m,e){
  var t=document.getElementById('toast');
  t.textContent=m; t.className='show'+(e?' err':'');
  clearTimeout(tT); tT=setTimeout(function(){t.className=''},3000);
}

var nightOn=false,recOn=false,aecAuto=true,agcAuto=true;
var recInt=null,recSecs=0;

function toggleNight(){
  nightOn=!nightOn;
  api('/night?v='+(nightOn?1:0)).then(function(d){
    nightOn=!!d.night;
    document.getElementById('btn-night').classList.toggle('on',nightOn);
    toast(nightOn?'Night ON':'Night OFF');
  });
}
function toggleAEC(){
  aecAuto=!aecAuto;
  api('/set?aec='+(aecAuto?1:0)).then(function(){
    document.getElementById('btn-aec').classList.toggle('on',aecAuto);
    toast('AEC '+(aecAuto?'AUTO':'MANUAL'));
  });
}
function toggleAGC(){
  agcAuto=!agcAuto;
  api('/set?agc='+(agcAuto?1:0)).then(function(){
    document.getElementById('btn-agc').classList.toggle('on',agcAuto);
    toast('AGC '+(agcAuto?'AUTO':'MANUAL'));
  });
}
function toggleRec(){
  var b=document.getElementById('btn-rec'),rb=document.getElementById('rb');
  rlog('TAP rec (recOn='+recOn+')');
  if(recOn){
    b.disabled=true; toast('Finalising MP4...');
    api('/rec/stop').then(function(d){
      b.disabled=false; recOn=false;
      clearInterval(recInt); recInt=null;
      rb.classList.remove('on');
      b.innerHTML='&#9210; REC'; b.classList.remove('ron');
      toast('Saved: '+(d.result||''));
    }).catch(function(){b.disabled=false;rlog('REC stop fetch failed');toast('Stop failed',true)});
  } else {
    api('/rec/start').then(function(d){
      if(!d.ok){rlog('REC refused '+(d.err||'?'));toast(d.err||'Cannot start',true);return}
      recOn=true; recSecs=0;
      toast('Recording \u2014 the live preview slows to keep the clip smooth');
      rb.classList.add('on');
      b.innerHTML='&#9209; STOP'; b.classList.add('ron');
      recInt=setInterval(function(){
        recSecs++;
        var m=String(Math.floor(recSecs/60)),s=String(recSecs%60);
        if(m.length<2)m='0'+m; if(s.length<2)s='0'+s;
        document.getElementById('rt').textContent=m+':'+s;
      },1000);
      toast('REC '+(d.file||''));phase('INSCRIBING');
    }).catch(function(){toast('No response',true)});
  }
}
/* ── Save to device ─────────────────────────────────────────────── */
function stamp(){
  var d=new Date(),p=function(n){return(n<10?'0':'')+n};
  return d.getFullYear()+p(d.getMonth()+1)+p(d.getDate())+'_'+
         p(d.getHours())+p(d.getMinutes())+p(d.getSeconds());
}
function closeVov(){
  var o=document.getElementById('vov'),v=document.getElementById('vpl');
  o.classList.remove('on'); v.pause();
  if(v.src){URL.revokeObjectURL(v.src); v.removeAttribute('src'); v.load();}
  ovBlob=null;
}
var ovBlob=null;
function saveOverlayVideo(){
  if(!ovBlob){toast('Nothing to save',true);return}
  if(ovBlob.size>2*1024*1024){
    toast('Too large for the board — long-press the video instead',true);return;
  }
  toast('Transferring '+Math.round(ovBlob.size/1024)+' KB...');
  var ext=(ovBlob.type.indexOf('mp4')>=0)?'mp4':'webm';
  stash(ovBlob,'WUWCAM_'+stamp()+'.'+ext,function(ok){
    if(ok){ closeVov(); serverDownload(); }
    else toast('Transfer failed',true);
  });
}
function showVideo(blob){
  ovBlob=blob;
  var v=document.getElementById('vpl');
  v.src=URL.createObjectURL(blob);
  document.getElementById('vov').classList.add('on');
}
function fallbackDl(blob,name){
  var u=URL.createObjectURL(blob);
  var a=document.createElement('a');
  a.href=u;a.download=name;
  document.body.appendChild(a);a.click();a.remove();
  setTimeout(function(){URL.revokeObjectURL(u)},10000);
}
/* Park the file in the board's PSRAM. Safari honours a server-sent
   Content-Disposition even on plain HTTP, which is the only reliable way to
   get a browser-generated file into iOS Files. */
/* Where a saved file ends up depends on how it is delivered. An "attachment" goes into the Files
   app on an iPhone. The same bytes served inline open in Safari's own viewer, and its share sheet
   has Save Image / Save Video, which put it straight into the Photos gallery. So on iOS the file is
   opened inline; elsewhere it downloads and the phone's gallery picks it up from Downloads. */
var IOS=/iPad|iPhone|iPod/.test(navigator.userAgent)||(navigator.platform==='MacIntel'&&navigator.maxTouchPoints>1);
function serverDownload(){
  if(!stashReady){ toast('Still transferring — try again in a moment',true); return; }
  var url='/dl?k='+stashTok+'&t='+Date.now()+(IOS?'&inline=1':'');
  var a=document.createElement('a');
  a.href=url; a.target='_blank'; a.rel='noopener';
  document.body.appendChild(a); a.click(); a.remove();
  toast(IOS?'Opened — tap the share icon, then Save to Photos':'Saved — find it in your gallery or Downloads');
}
/* iOS will not "Save to Photos" a video, but it will for an image shown on the
   page. So for stills we present the picture and let the native long-press do
   the work — no download manager, no popup, no permissions. */
function offerImage(blob, note){
  var v=document.getElementById('sim');
  if(v.src) URL.revokeObjectURL(v.src);
  v.src=URL.createObjectURL(blob);
  document.getElementById('shint').innerHTML =
    (IOS?'Long-press the image &rarr; <b>Add to Photos</b>'
        :'Press and hold the image &rarr; <b>Save image</b> (it lands in your gallery)') +
    (note ? ('<br>'+note) : '');
  document.getElementById('sov').classList.add('on');
}
var stashReady=false;          // is the file on the board actually ours yet?
var stashTok=0;                // proves the stash is still OURS at /dl time
function stash(blob,name,cb){
  stashReady=false; stashTok=0;
  var mime=(blob.type||'application/octet-stream').split(';')[0].replace('/','-');
  fetch('/upload?name='+encodeURIComponent(name)+'&mime='+encodeURIComponent(mime),
        {method:'POST',body:blob})
    .then(function(r){return r.json()})
    .then(function(d){
      stashReady=!!(d&&d.ok);
      stashTok=(d&&d.tok)||0;
      if(!stashReady) rlog('STASH failed '+((d&&d.err)||'?'));
      cb(stashReady, d&&d.err);
    })
    .catch(function(){ rlog('STASH network fail'); cb(false,'net'); });
}
function dl(blob,ext){
  var name='WUWCAM_'+stamp()+'.'+ext;
  if(navigator.share&&navigator.canShare){          // desktop / HTTPS path
    var f=null;
    try{ f=new File([blob],name,{type:blob.type}); }catch(e){}
    if(f&&navigator.canShare({files:[f]})){
      navigator.share({files:[f]})
        .then(function(){toast('Saved '+ext.toUpperCase())})
        .catch(function(e){ if(!e||e.name!=='AbortError') fallbackDl(blob,name); });
      return;
    }
  }
  fallbackDl(blob,name);
  toast('Saved '+ext.toUpperCase()+' ('+Math.round(blob.size/1024)+' KB)');
}
/* Motion trigger: the camera decides when to shoot. Frames are downsampled to
   a 32x24 thumbnail and compared; when enough changes, the shutter fires. */
var mtOn=false,mtPrev=null,mtCv=null,mtCx=null,mtCool=0;
function motionCheck(){
  if(!mtOn||!img.naturalWidth)return;
  if(!mtCv){mtCv=document.createElement('canvas');mtCv.width=32;mtCv.height=24;
            mtCx=mtCv.getContext('2d',{willReadFrequently:true});}
  mtCx.drawImage(img,0,0,32,24);
  var d=mtCx.getImageData(0,0,32,24).data,sum=0;
  if(mtPrev){
    for(var i=0;i<d.length;i+=4) sum+=Math.abs(d[i]-mtPrev[i]);
    var score=sum/(32*24);
    document.getElementById('mtv').textContent=score.toFixed(0);
    if(score>mtThresh&&Date.now()>mtCool){
      mtCool=Date.now()+2500;
      if(sdPresent){
        api('/capture'+(pasarOn?('?as='+encodeURIComponent(pasarMe)):'')).then(function(r){
          if(pasarOn && r && String(r.result||'').indexOf('ERR')<0) pasarSendView();
          toast('Motion '+score.toFixed(0)+' — saved '+(r.result||''));
        });
      } else {
        mtHits++;
        toast('Motion '+score.toFixed(0)+' ('+mtHits+') — insert SD to record');
      }
    }
  }
  mtPrev=new Uint8ClampedArray(d);
}
var mtThresh=14,mtHits=0,sdPresent=false;
function toggleMotion(){
  mtOn=!mtOn;mtPrev=null;
  document.getElementById('btn-mt').classList.toggle('on',mtOn);
  toast(mtOn?'Motion trigger ARMED':'Motion trigger off');phase(mtOn?'WATCHING':'ATTENDING');
}
setInterval(motionCheck,400);

/* Exposure engine control */
function expStart(){
  if(!gl){toast('Needs GPU FX (WebGL)',true);return}
  accMode=parseInt(document.getElementById('sel-exp').value,10);
  accDur=parseInt(document.getElementById('sel-expd').value,10);
  accN=0;accK=1;accFrozen=false;accStart=Date.now();accOn=true;
  accAlloc();
  // clear both accumulator buffers to the right starting state
  var clr=(accMode===92)?1:0;                 // DARKEN starts white
  for(var j=0;j<2;j++){
    gl.bindFramebuffer(gl.FRAMEBUFFER,accFbo[j]);
    gl.clearColor(clr,clr,clr,1);gl.clear(gl.COLOR_BUFFER_BIT);
  }
  gl.bindFramebuffer(gl.FRAMEBUFFER,null);
  document.getElementById('btn-exp').innerHTML='&#9209; DEVELOP';
  document.getElementById('btn-exp').classList.add('ron');
  toast('Exposing...');phase('EXPOSING');
}
function expStop(){
  accFrozen=true;
  document.getElementById('btn-exp').innerHTML='&#11015; KEEP';
  document.getElementById('btn-exp').classList.remove('ron');
  document.getElementById('btn-exp').classList.add('on');
  toast('Exposure held ('+accN+' frames) — tap KEEP');phase('FIXED');
}
function expKeep(){
  var c=document.createElement('canvas');
  c.width=gc.width;c.height=gc.height;
  c.getContext('2d').drawImage(gc,0,0);
  c=rotated(c,viewTurn());
  c.toBlob(function(bb){
    if(!bb){toast('Capture failed',true);return}
    toast('Transferring '+Math.round(bb.size/1024)+' KB...');
    stash(bb,'WUWCAM_EXP_'+stamp()+'.png',function(ok){
      if(ok) serverDownload();
      else   dl(bb,'png');
    });
  },'image/png');
  expReset();
}
function expReset(){
  accOn=false;accFrozen=false;accN=0;
  var b=document.getElementById('btn-exp');
  b.innerHTML='&#9673; EXPOSE';b.classList.remove('on','ron');phase('ATTENDING');
}
function toggleExp(){
  if(accOn&&accFrozen) return expKeep();
  if(accOn) return expStop();
  expStart();
}
/* Full-resolution FX still. The server-side 5MP JPEG has no effects, and the
   canvas capture is only stream-resolution — this closes that gap: pull the
   full-res frame, push it through the same shader on the GPU, save the result.
   Capped on the long edge so texture memory stays sane on a phone. */
var FXP_MAX=2048;
function fxPhoto(){ fxRenderURL('/photo?t='+Date.now(),'WUWCAM_FX_','Capturing full-res...'); }
function fxRenderURL(srcUrl,prefix,note){
  if(!gl||!glReady){toast('Needs GPU FX',true);return}
  toast(note||'Rendering...');
  var wasRun=run; run=false; clearTimeout(tmr); clearTimeout(wd); busy=false;
  setTimeout(function(){ run=wasRun; if(run){busy=false;nextFrame();} }, 2500);
  var turns=viewTurn(), exifApplied=false;
  fetch(srcUrl).then(function(r){
    if(!r.ok)throw 0; return r.blob();
  }).then(function(b){
    /* Stills made by this camera say which way up they are (EXIF) and the browser turns them while
       decoding; older ones do not, and those get the page's own turn when the result is saved. */
    return new Promise(function(res){
      var fr=new FileReader();
      fr.onload=function(){ exifApplied=exifTurns(fr.result)>0; res(b); };
      fr.onerror=function(){ res(b); };
      fr.readAsArrayBuffer(b.slice(0,65536));
    });
  }).then(function(b){
    return new Promise(function(res,rej){
      var im=new Image();
      im.onload=function(){res(im)}; im.onerror=rej;
      im.src=URL.createObjectURL(b);
    });
  }).then(function(im){
    var w=im.naturalWidth,h=im.naturalHeight,sc=Math.min(1,FXP_MAX/Math.max(w,h));
    w=Math.round(w*sc)&~1; h=Math.round(h*sc)&~1;
    toast('Rendering '+w+'x'+h+'...');
    var pTex=null,tFbo=null,tTex=null,ok=false;
    try{
      gc.width=w; gc.height=h;
      pTex=mkTex();
      gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,true);
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D,pTex);
      gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,im);
      tTex=mkTex(w,h);
      tFbo=gl.createFramebuffer();
      gl.bindFramebuffer(gl.FRAMEBUFFER,tFbo);
      gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,tTex,0);
      if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE)
        throw new Error('fbo');
      gl.bindFramebuffer(gl.FRAMEBUFFER,null);
      if(fxN===11&&pcP) pcPass(tFbo,pTex);
      else              glPass(tFbo,fxN,pTex,pTex);
      glPass(null,finalFx(),tTex,tTex);   // plain copy unless an effect or grain was asked for
      ok=true;
    }catch(e){ toast('Render failed: '+(e.name||e),true); }
    URL.revokeObjectURL(im.src);
    if(ok){
      var outCv=rotated(gc,exifApplied?0:turns);
      var finish=function(bb){
        cleanupFxp(pTex,tFbo,tTex);
        if(!bb){toast('Encode failed',true);return}
        toast('Transferring '+Math.round(bb.size/1024)+' KB...');
        stash(bb,prefix+stamp()+'.png',function(good){
          if(good) serverDownload();
          else     dl(bb,'png');
        });
      };
      outCv.toBlob(finish,'image/png');
    } else cleanupFxp(pTex,tFbo,tTex);
  }).catch(function(){ toast('Full-res capture failed',true); });
}
function cleanupFxp(pTex,tFbo,tTex){
  try{
    if(pTex)gl.deleteTexture(pTex);
    if(tTex)gl.deleteTexture(tTex);
    if(tFbo)gl.deleteFramebuffer(tFbo);
  }catch(e){}
  gc.width=1;                 // force glSize() to rebuild for the live stream
}
function saveJpg(){
  rlog('TAP 5mp');
  toast('Fetching maximum-resolution photo...');
  var wasRun=run; run=false; clearTimeout(tmr); clearTimeout(wd); busy=false;
  function resume(){ run=wasRun; if(run){ busy=false; nextFrame(); } }
  fetch('/photo').then(function(r){
    if(!r.ok)throw 0; return r.blob();
  }).then(function(b){
    resume();
    offerImage(b, Math.round(b.size/1024)+' KB');
    stash(b,'WUWCAM_'+stamp()+'.jpg',function(){});
  }).catch(function(){ resume(); rlog('Maximum-resolution fetch failed'); toast('Photo failed',true); });
}
function savePng(){
  if(!img.naturalWidth){toast('No frame yet',true);return}
  var s=fxSrc();                       // processed canvas when GPU FX active
  var c=document.createElement('canvas');
  c.width=gl?gc.width:img.naturalWidth;
  c.height=gl?gc.height:img.naturalHeight;
  c.getContext('2d').drawImage(s,0,0);
  c=rotated(c,viewTurn());
  c.toBlob(function(bb){
    if(!bb){toast('PNG failed',true);return}
    offerImage(bb, 'or tap SAVE TO GALLERY below');
    stash(bb,'WUWCAM_'+stamp()+'.png',function(){});
  },'image/png');
}

/* In-browser video recording: frames are drawn to a canvas as they stream,
   MediaRecorder encodes on the device. iOS/iPad Safari → real H.264 .mp4;
   Chrome/Firefox → .webm. Zero extra load on the camera board.
   Safari rules learned the hard way:
     · start() WITHOUT a timeslice — chunked mp4 recording produces broken files
     · captureStream() WITHOUT an fps argument — the fps variant is flaky
     · keep the canvas repainting steadily so the encoder always sees frames */
var dRec=null,dChunks=[],dCanvas=null,dCtx=null,dTimer=null,dSecs=0,dDraw=null,dRot=0;
/* One frame of the recording canvas: the live picture turned the way the viewer sees it. */
function drawRec(){
  var src=fxSrc(), cw=dCanvas.width, ch=dCanvas.height;
  if(!dRot){ dCtx.drawImage(src,0,0,cw,ch); return; }
  var q=dRot&1, dw=q?ch:cw, dh=q?cw:ch;
  dCtx.save(); dCtx.translate(cw/2,ch/2); dCtx.rotate(dRot*Math.PI/2);
  dCtx.drawImage(src,-dw/2,-dh/2,dw,dh); dCtx.restore();
}
var pendBlob=null,pendExt='',pendReady=false;
function pickMime(){
  if(!window.MediaRecorder||!MediaRecorder.isTypeSupported)return '';
  var l=['video/mp4;codecs=avc1.42E01E','video/mp4',
         'video/webm;codecs=vp9','video/webm;codecs=vp8','video/webm'];
  for(var i=0;i<l.length;i++) if(MediaRecorder.isTypeSupported(l[i])) return l[i];
  return '';
}
function devRecCleanup(b){
  clearInterval(dTimer); dTimer=null;
  clearInterval(dDraw);  dDraw=null;
  dRec=null; dCtx=null;
  b.innerHTML='&#9210; MP4'; b.classList.remove('ron');
}
function toggleDevRec(){
  var b=document.getElementById('btn-drec');
  if(pendReady){                      // file already parked on the board
    pendReady=false; pendBlob=null; pendExt='';
    b.innerHTML='&#9210; MP4'; b.classList.remove('on');
    toast('Opening download...');
    serverDownload();   // server Content-Disposition
    return;
  }
  if(dRec){ try{dRec.stop()}catch(e){devRecCleanup(b)} return; }
  if(!window.MediaRecorder){toast('This browser has no MediaRecorder',true);return}
  if(!img.naturalWidth){toast('No frame yet',true);return}

  dRot=viewTurn();                                   // one clip, one orientation: fixed at the start
  dCanvas=document.createElement('canvas');
  var srcW=gl?gc.width:img.naturalWidth, srcH=gl?gc.height:img.naturalHeight;
  dCanvas.width=((dRot&1)?srcH:srcW)&~1;             // H.264 needs even dimensions
  dCanvas.height=((dRot&1)?srcW:srcH)&~1;
  dCtx=dCanvas.getContext('2d');
  drawRec();

  var stream;
  try{ stream=dCanvas.captureStream(); }
  catch(e){ toast('captureStream: '+e.name,true); dCtx=null; return; }

  var mime=pickMime();
  // ~2.5 Mbps keeps ~12 s inside the board's 4 MB buffer, which is the only
  // route that reaches iOS Files. Without a cap browsers pick 8 Mbps+.
  var mrOpt={videoBitsPerSecond:2500000};
  if(mime)mrOpt.mimeType=mime;
  try{ dRec=new MediaRecorder(stream,mrOpt); }
  catch(e0){ try{ dRec=new MediaRecorder(stream); }catch(e1){ dRec=null; } }
  if(!dRec){ toast('Recorder unavailable',true); dCtx=null; return; }
  try{ 
  }catch(e){ toast('Recorder: '+e.name,true); dRec=null; dCtx=null; return; }

  dChunks=[]; dSecs=0;
  dRec.ondataavailable=function(e){ if(e.data&&e.data.size)dChunks.push(e.data) };
  dRec.onerror=function(e){ toast('REC error: '+(e.error?e.error.name:'unknown'),true) };
  dRec.onstop=function(){
    var mt=(dRec&&dRec.mimeType)||mime||'video/webm';
    var ext=mt.indexOf('mp4')>=0?'mp4':'webm';
    devRecCleanup(b);
    if(!dChunks.length){ toast('No video data captured',true); return; }
    var bl=new Blob(dChunks,{type:mt}); dChunks=[];
    pendBlob=bl; pendExt=ext;
    var kb=Math.round(bl.size/1024);
    if(bl.size>2*1024*1024){          // larger than the board can hold
      toast(kb+' KB — too big to stash, showing player',true);
      pendBlob=null; showVideo(bl); return;
    }
    toast('Transferring '+kb+' KB...');
    stash(bl,'WUWCAM_'+stamp()+'.'+ext,function(ok){
      if(ok){ pendReady=true;
        b.innerHTML='&#11015; SAVE '+ext.toUpperCase();
        b.classList.add('on');
        toast('Ready — tap SAVE ('+kb+' KB)');
      } else { pendBlob=null; toast('Transfer failed — using player',true); showVideo(bl); }
    });
  };

  // The rAF loop repaints dCanvas every display frame, so the encoder gets a
  // steady 60 fps source even when the network stalls. This interval is only a
  // safety net for browsers that throttle rAF.
  dDraw=setInterval(function(){
    if(dCtx&&img.naturalWidth) drawRec();
  },250);

  try{ dRec.start(); }   // single blob at stop — the Safari-safe mode
  catch(e){ toast('start: '+e.name,true); devRecCleanup(b); return; }

  b.classList.add('ron');
  dTimer=setInterval(function(){ dSecs++; b.innerHTML='&#9209; '+dSecs+'s'; },1000);
  toast('REC ('+(mime||'browser default')+')');
}

function doCapture(){
  rlog('TAP capture');
  toast('Capturing maximum resolution...');
  // esp_http_server serves ONE request at a time and the viewfinder polls with
  // no gap when you are alone, so it can starve a slow request like this one.
  // Stand the stream down for the ~1 s the 5 MP capture needs.
  var wasRun=run; run=false; clearTimeout(tmr); clearTimeout(wd); busy=false;
  function resume(){ run=wasRun; if(run){ busy=false; nextFrame(); } }
  api('/capture').then(function(d){
    resume();
    if(d.result&&d.result.indexOf('ERR')!==0){
      toast(d.result);
      document.getElementById('lp').textContent=d.count+' IMG';
      loadGallery(false); loadFiles();
    } else { rlog('CAPTURE err '+(d.result||'?')); toast(d.result||'Fail',true); }
  }).catch(function(){ resume(); rlog('CAPTURE fetch failed'); toast('No response',true); });
}
var RES={5:'QVGA',8:'VGA',9:'SVGA',10:'XGA',11:'HD',12:'SXGA',13:'UXGA',14:'FHD',17:'QXGA',18:'QHD',21:'QSXGA'};
function onRes(sel){
  api('/set?res='+sel.value).then(function(){
    document.getElementById('lr').textContent=RES[sel.value]||sel.value;
    toast('Res: '+(RES[sel.value]||sel.value));
  });
}
function onPhotoRes(sel){
  api('/set?photores='+sel.value).then(function(){
    toast('Still: '+(RES[sel.value]||sel.value));
  });
}
var camMirror=true,camVFlip=false;
function orientUi(){
  var m=document.getElementById('btn-mirror'),f=document.getElementById('btn-vflip');
  if(m){m.textContent='MIRROR '+(camMirror?'ON':'OFF');m.classList.toggle('on',camMirror)}
  if(f){f.textContent='FLIP '+(camVFlip?'ON':'OFF');f.classList.toggle('on',camVFlip)}
}
function toggleMirror(){
  camMirror=!camMirror;orientUi();api('/set?hmirror='+(camMirror?1:0));
}
function toggleVFlip(){
  camVFlip=!camVFlip;orientUi();api('/set?vflip='+(camVFlip?1:0));
}
function recCfg(){
  api('/rec/config?fps='+document.getElementById('sel-rfps').value+
      '&effect='+document.getElementById('sel-rfx').value+
      '&intensity='+document.getElementById('sl-ri').value+
      '&delay='+document.getElementById('sl-rd').value);
}
/* ── SHARED SESSION ──────────────────────────────────────────────────
   Everyone here sees the same viewfinder. Anyone can fire the shutter, and
   the frame lands in a gallery all of us can pull from. The look is applied
   on YOUR device, so the same instant can go home with each of us rendered
   completely differently. */
var myName='', lastGen=-1, shots=[], curShot=0;
try{ myName=localStorage.getItem('wuwname')||''; }catch(e){}
if(!myName) myName='guest-'+Math.floor(Math.random()*900+100);
function setName(v){
  v=(v||'').trim(); if(!v)return;
  myName=v.slice(0,16);
  try{localStorage.setItem('wuwname',myName)}catch(e){}
  toast('You are '+myName); syncLoop();
}
function renderWho(w){
  w=w||[];
  nHere=w.length||1; applyRate();
  var el=document.getElementById('who');
  if(el) el.textContent = w.length ? w.join('  \u00b7  ') : 'just you';
  var n=document.getElementById('whoN'); if(n) n.textContent=w.length||1;
}
function drawStrip(){
  var el=document.getElementById('strip'); if(!el)return;
  if(!shots.length){ el.innerHTML='<div class="lbl">no shots yet</div>'; return; }
  var h='';
  for(var i=0;i<shots.length;i++){
    var s=shots[i];
    h+='<div class="tile" onclick="openShot('+s.id+')">'+
       '<img loading="lazy" src="'+shotURL(s.id,false)+'" alt="">'+
       '<span>'+s.by+'</span></div>';
  }
  el.innerHTML=h;
}
function loadGallery(quiet){
  Promise.all([
    fetch('/gallery').then(function(r){return r.json()}).catch(function(){return {shots:[]}}),
    fetch('/archive/list',{cache:'no-store'}).then(function(r){return r.json()}).catch(function(){return []})
  ]).then(function(all){
    var d=all[0]||{},archived=all[1]||[];
    var prevTop = shots.length ? shots[0].id : 0;
    shots = d.shots || [];
    archived.sort(function(a,b){return b.n-a.n});
    for(var i=0;i<archived.length&&i<24;i++)
      shots.push({id:-(archived[i].n+1),n:archived[i].n,sd:true,
                  by:'SD '+String(archived[i].n).padStart(4,'0')});
    drawStrip();
    if(!quiet && shots.length && shots[0].id>prevTop && shots[0].by!==myName)
      toast('\u25c9 '+shots[0].by+' captured');
  }).catch(function(){});
}
function syncLoop(){
  if(!myName)return;
  fetch('/sync?name='+encodeURIComponent(myName))
    .then(function(r){return r.json()})
    .then(function(d){
      renderWho(d.who);
      if(d.gen!==lastGen){ var first=(lastGen<0); lastGen=d.gen; loadGallery(first); }
    }).catch(function(){});
}
function fireShot(){
  rlog('TAP shoot');
  var b=document.getElementById('btn-shoot');
  if(b)b.disabled=true;
  fetch('/shot?by='+encodeURIComponent(myName))
    .then(function(r){return r.json()})
    .then(function(d){
      if(d.ok){
        if(b)b.disabled=false; toast('Captured #'+d.id); lastGen=-1; syncLoop(); return;
      }
      if(d.err!=='no PSRAM') throw new Error(d.err||'shot failed');
      return fetch('/capture?as='+encodeURIComponent(myName)).then(function(r){return r.json()}).then(function(s){
        if(b)b.disabled=false;
        if(!s.result||s.result.indexOf('ERR')===0) throw new Error(s.result||'shot failed');
        toast('Saved to SD'); loadGallery(false); loadFiles();
      });
    }).catch(function(e){ if(b)b.disabled=false; toast((e&&e.message)||'no response',true); });
}
function shotURL(id,full){
  for(var i=0;i<shots.length;i++)if(shots[i].id===id)
    return shots[i].sd?('/archive/img?n='+shots[i].n+(full?'&full=1':'')):('/frame?id='+id);
  return '/frame?id='+id;
}
function openShot(id){
  curShot=id;
  document.getElementById('sim').src=shotURL(id,true);
  var m=null;
  for(var i=0;i<shots.length;i++) if(shots[i].id===id) m=shots[i];
  document.getElementById('shint').textContent = m && m.sd ? m.by :
    (m ? ('#'+id+'  by '+m.by+'  \u00b7  '+m.kb+' KB  \u00b7  '+m.age+'s ago') : ('#'+id));
  document.getElementById('sov').classList.add('on');
}
function closeSov(){
  document.getElementById('sov').classList.remove('on');
  document.getElementById('sim').removeAttribute('src');
}
function shotMyFx(){
  var id=curShot; closeSov();
  fxRenderURL(shotURL(id,true),'WUWCAM_SHOT_','Rendering with your look...');
}
function shotSaveRaw(){
  var id=curShot;
  toast('Fetching shot...');
  fetch(shotURL(id,true)).then(function(r){return r.blob()}).then(function(b){
    stash(b,'WUWCAM_shot'+id+'.jpg',function(ok){
      if(ok){ closeSov(); serverDownload(); } else dl(b,'jpg');
    });
  }).catch(function(){ toast('fetch failed',true); });
}
function shotBwoWall(){
  var m=null;
  for(var i=0;i<shots.length;i++)if(shots[i].id===curShot)m=shots[i];
  if(!m||!m.sd||m.n==null){
    toast('Choose an SD photograph from the gallery',true); return;
  }
  fetch('/panel/bwo-wall?n='+m.n,{cache:'no-store'})
    .then(function(r){return r.json()}).then(function(d){
      if(!d.ok)throw new Error(d.err||'wall failed');
      closeSov(); toast('Photo added to BWO walls');
    }).catch(function(e){toast((e&&e.message)||'wall failed',true)});
}
function fileBwoWall(name){
  fetch('/panel/bwo-wall?f='+encodeURIComponent(name),{cache:'no-store'})
    .then(function(r){return r.json()}).then(function(d){
      if(!d.ok)throw new Error(d.err||'wall failed');
      toast('SD photograph added to BWO');
    }).catch(function(e){toast((e&&e.message)||'wall failed',true)});
}
// Kick these off HERE, where myName is guaranteed to hold a value.
var _ni=document.getElementById('nameIn'); if(_ni)_ni.value=myName;
var _sk=document.getElementById('sel-skin'); if(_sk)_sk.value=SKIN;
syncLoop();
setInterval(syncLoop,2500);

/* Pull recordings and photos off the card over WiFi. The download runs on
   :81 because it monopolises whichever server serves it, and :80 has to stay
   responsive for the live view and the shared session. */
function loadFiles(){
  var el=document.getElementById('files');
  el.textContent='reading card...';
  fetch('/files?t='+Date.now(),{cache:'no-store'}).then(function(r){return r.json()}).then(function(d){
    if(!d.ok){ el.textContent=d.err||'card error'; return; }
    if(!d.files.length){ el.textContent='card is empty'; return; }
    var host=location.hostname, h='<div class="sdgrid">';
    for(var i=0;i<d.files.length;i++){
      var f=d.files[i];
      var u='http://'+host+':81/sd?f='+encodeURIComponent(f.n);
      if(f.kind==='jpg')
        h+='<a class="sditem" target="_blank" rel="noopener" href="'+u+'">'+
           '<img loading="lazy" decoding="async" src="'+
           (f.i!=null?('/archive/img?n='+f.i):u)+'" alt=""><span>'+f.n+'</span></a>';
      else
        h+='<a class="sditem video" target="_blank" rel="noopener" href="'+u+'">&#9654;<span>'+f.n+'</span></a>';
    }
    h+='</div>';
    for(var i=0;i<d.files.length;i++){
      var f=d.files[i],u='http://'+host+':81/sd?f='+encodeURIComponent(f.n);
      h+='<div class="frow"><span>'+f.n+'</span>'+
         '<b>'+f.kb+' KB</b>'+
         (f.kind==='jpg'?'<button class="dl" title="Use photograph as a BWO material" onclick="fileBwoWall(\''+f.n+'\')">\u25a3</button>':'<span></span>')+
         saveLink(u)+'</div>';
    }
    el.innerHTML=h;
  }).catch(function(){ el.textContent='no response'; });
}
/* SAVE on a card file. iPhone: open it inline (photo viewer or video player) and use Share -> Save to
   Photos. Android and desktop: a real download, which Gallery apps list from Downloads. The :81 server
   answers byte-range requests, which is what lets Safari play and save a video at all. */
function saveLink(u){
  return IOS
    ? '<a class="dl" target="_blank" rel="noopener" href="'+u+'" title="Opens it: tap Share, then Save to Photos">&#11015; SAVE</a>'
    : '<a class="dl" href="'+u+'&dl=1" title="Saves to this device, into your gallery">&#11015; SAVE</a>';
}
function getFile(n){
  var fr=document.getElementById('dlframe');
  if(!fr){ fr=document.createElement('iframe'); fr.id='dlframe';
           fr.style.display='none'; document.body.appendChild(fr); }
  fr.src='http://'+location.hostname+':81/sd?f='+encodeURIComponent(n);
  toast('Downloading '+n);
}

/* ── network + firmware updates ──────────────────────────────────────
   The camera stays an access point the whole time; joining your WiFi is
   only so it can reach the update server. Both radios share one channel,
   so the AP may blink as it follows your router — that is expected. */
var netScanTimer=null,netPollTimer=null;
function pickNet(v){
  if(v)document.getElementById('wSsid').value=v;
  document.getElementById('wPass').focus();
}
function scanNet(){
  clearTimeout(netScanTimer);
  var b=document.getElementById('btn-scan');b.disabled=true;b.textContent='SCANNING...';
  fetch('/net/scan?t='+Date.now(),{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(d){
      if(d&&d.scanning){netScanTimer=setTimeout(scanNet,700);return}
      b.disabled=false;b.textContent='SCAN NEARBY';
      var sel=document.getElementById('wScan');sel.innerHTML='';
      var nets=(d&&d.nets)||[];
      if(!nets.length){var z=document.createElement('option');z.textContent='No networks found';sel.appendChild(z);return}
      for(var i=0;i<nets.length;i++){
        var o=document.createElement('option');o.value=nets[i].ssid;
        o.textContent=nets[i].ssid+'  '+nets[i].rssi+' dBm'+(nets[i].secure?'  locked':'  open');
        sel.appendChild(o);
      }
      pickNet(sel.value);
    }).catch(function(){b.disabled=false;b.textContent='SCAN NEARBY';toast('WiFi scan failed',true)});
}
function pollNet(left){
  clearTimeout(netPollTimer);
  fetch('/net?t='+Date.now(),{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(d){
      var s=document.getElementById('netState');
      if(d&&d.online){s.textContent='connected · '+d.ssid+' · '+d.ip;toast('Online');return}
      s.textContent='connecting to '+((d&&d.ssid)||'network')+'...';
      if(left>0)netPollTimer=setTimeout(function(){pollNet(left-1)},1000);
      else toast('Could not connect',true);
    }).catch(function(){if(left>0)netPollTimer=setTimeout(function(){pollNet(left-1)},1000)});
}
function saveNet(){
  var ss=document.getElementById('wSsid').value.trim();
  var pw=document.getElementById('wPass').value;
  if(!ss){ toast('Enter a network name',true); return; }
  document.getElementById('netState').textContent='connecting...';
  api('/net?ssid='+encodeURIComponent(ss)+'&pass='+encodeURIComponent(pw))
    .then(function(){pollNet(15)}).catch(function(){toast('Connect request failed',true)});
}
(function(){
  var p=document.getElementById('wPass');
  if(p)p.addEventListener('keydown',function(e){if(e.key==='Enter'){e.preventDefault();saveNet()}});
})();
function otaCheck(){
  document.getElementById('otaState').textContent='checking...';
  api('/ota/check').then(function(d){
    if(!d.ok){ document.getElementById('otaState').textContent=d.err||'check failed';
               toast(d.err||'Check failed',true); return; }
    if(d.update){
      document.getElementById('otaState').innerHTML =
        'v'+d.current+' &rarr; <b>v'+d.latest+'</b>'+(d.notes?('<br>'+d.notes):'');
      document.getElementById('otaRow').style.display='';
      toast('Update available: v'+d.latest);
    } else {
      document.getElementById('otaState').textContent='v'+d.current+' — up to date';
      document.getElementById('otaRow').style.display='none';
      toast('Already up to date');
    }
  });
}
function otaApply(){
  var b=document.getElementById('btn-ota');
  b.disabled=true; b.innerHTML='&#8681; INSTALLING...';
  document.getElementById('otaState').textContent =
    'downloading — do not unplug. The camera reboots when done.';
  api('/ota/apply').then(function(d){
    if(!d.ok){ b.disabled=false; b.innerHTML='&#8681; INSTALL UPDATE';
               toast(d.err||'Update failed',true);
               document.getElementById('otaState').textContent=d.err||'failed';
               return; }
    // the board reboots mid-flash, so poll until it answers again
    var tries=0;
    var iv=setInterval(function(){
      tries++;
      fetch('/status?t='+Date.now()).then(function(r){return r.json()}).then(function(){
        clearInterval(iv); toast('Updated — reloading'); location.reload();
      }).catch(function(){
        if(tries>40){ clearInterval(iv);
          document.getElementById('otaState').textContent=
            'reboot is taking a while — reconnect to the wuw network and reload'; }
      });
    },3000);
  });
}

function pollStatus(){
  api('/status').then(function(d){
    if(!d||typeof d.sd==='undefined')return;
    /* Firmware changed while this page stayed open -> the DOM and JS are from
       a build that no longer exists. Reload once, then leave it alone. */
    if(d.build){
      var seen=null;
      try{ seen=sessionStorage.getItem('wuwbuild'); }catch(e){}
      if(!seen){ try{ sessionStorage.setItem('wuwbuild',d.build); }catch(e){} }
      else if(seen!==d.build){
        var done=null;
        try{ done=sessionStorage.getItem('wuwreloaded'); }catch(e){}
        if(!done){
          try{ sessionStorage.setItem('wuwreloaded','1');
               sessionStorage.setItem('wuwbuild',d.build); }catch(e){}
          rlog('STALE PAGE -> reloading for build '+d.build);
          location.reload();
          return;
        }
      }
    }
    sdPresent=!!d.sd;
    document.getElementById('lsd').textContent=d.sd?'OK':'--';
    document.getElementById('lr').textContent=RES[d.res]||d.res;
    var sr=document.getElementById('sel-res');if(sr)sr.value=String(d.res);
    var pr=document.getElementById('sel-photores');if(pr)pr.value=String(d.photoRes);
    if(d.maxRes!=null){
      [sr,pr].forEach(function(sel){
        if(!sel)return;
        for(var i=0;i<sel.options.length;i++)
          sel.options[i].disabled=(+sel.options[i].value>d.maxRes);
      });
    }
    var cm=document.getElementById('cam-model');if(cm)cm.textContent=d.sensor||'camera';
    var sq=document.getElementById('sl-streamq');if(sq)sq.value=String(d.streamQ);
    var pq=document.getElementById('sl-photoq');if(pq)pq.value=String(d.photoQ);
    var br=document.getElementById('sl-bright');if(br)br.value=String(d.bright);
    var vq=document.getElementById('vq');if(vq)vq.textContent=d.streamQ;
    var vpq=document.getElementById('vpq');if(vpq)vpq.textContent=d.photoQ;
    var vb=document.getElementById('vbright');if(vb)vb.textContent=(d.bright>0?'+':'')+d.bright;
    [['sl-ae','vael',d.aeLevel],['sl-contrast','vco',d.contrast]].forEach(function(item){
      if(item[2]==null)return;
      var input=document.getElementById(item[0]), output=document.getElementById(item[1]);
      if(input && document.activeElement!==input)input.value=String(item[2]);
      if(output && document.activeElement!==input)output.textContent=String(item[2]);
    });
    camMirror=!!d.hmirror;camVFlip=!!d.vflip;orientUi();
    if(d.viewrot!=null&&(d.viewrot&3)!==viewTurn()){ viewRot=d.viewrot&3; applyViewRot(); }
    recBusy=!!d.rec;
    var wbs=document.getElementById('sel-wb');if(wbs&&d.wb!=null&&document.activeElement!==wbs)wbs.value=String(d.wb);
    var sas=document.getElementById('sl-sat'),vsa=document.getElementById('vsa');
    if(sas&&d.sat!=null&&document.activeElement!==sas){ sas.value=String(d.sat); if(vsa)vsa.textContent=String(d.sat); }
    if(d.apCustom!=null) apShowState(!!d.apCustom);
    /* no SD → dim capture/rec so it's obvious they need a card; stream is unaffected */
    document.getElementById('btn-cap').style.opacity=d.sd?'':'0.35';
    document.getElementById('btn-rec').style.opacity=d.sd?'':'0.35';
    if(d.photos>0)document.getElementById('lp').textContent=d.photos+' IMG';
    if(d.cam===false){
      run=false; clearTimeout(tmr); clearTimeout(wd);
      se.innerHTML='CAMERA NOT DETECTED<br>check ribbon seating';
      se.classList.add('on');
    }
    if(!d.rec&&recOn){
      recOn=false; clearInterval(recInt); recInt=null;
      document.getElementById('rb').classList.remove('on');
      var b=document.getElementById('btn-rec');
      b.innerHTML='&#9210; REC'; b.classList.remove('ron');
      toast('Auto-stopped');
    }
  });
}
/* ── The camera's WiFi password ─────────────────────────────────────────── */
var apIsCustom=null, apSsid='wuw';
fetch('/ap',{cache:'no-store'}).then(function(r){return r.json()}).then(function(d){ if(d&&d.ssid) apSsid=d.ssid; }).catch(function(){});
function apShowState(custom){
  apIsCustom=custom;
  var st=document.getElementById('apState'), hint=document.getElementById('ap-hint');
  if(st) st.innerHTML=custom
    ? 'You have set your own password. Only people who know it can join this camera.'
    : '<b style="color:var(--blood)">Still using the factory password</b>, which is printed in the manual. Set your own below.';
  if(hint) hint.textContent=custom?'the one you set':'wuwuwuwu (factory)';
  var cur=document.getElementById('apCur');
  if(cur&&!custom&&!cur.value) cur.placeholder='current password (factory: wuwuwuwu)';
}
function apShow(on){
  ['apCur','apNew','apNew2'].forEach(function(id){ var e=document.getElementById(id); if(e) e.type=on?'text':'password'; });
}
function apChange(){
  var cur=document.getElementById('apCur').value, n1=document.getElementById('apNew').value,
      n2=document.getElementById('apNew2').value, b=document.getElementById('btn-ap');
  if(!cur){ toast('Enter the current password',true); return; }
  if(n1.length<8){ toast('The new password needs at least 8 characters',true); return; }
  if(n1!==n2){ toast('The two new passwords differ',true); return; }
  if(cur.indexOf('\n')>=0||n1.indexOf('\n')>=0){ toast('No line breaks in a password',true); return; }
  b.disabled=true; b.textContent='CHANGING...';
  fetch('/ap/pass',{method:'POST',headers:{'X-Wuw-Ap':'1','Content-Type':'text/plain'},body:cur+'\n'+n1})
    .then(function(r){return r.json()})
    .then(function(d){
      b.disabled=false; b.textContent='SET PASSWORD';
      if(!d||!d.ok){ toast((d&&d.err)||'Could not change it',true); return; }
      ['apCur','apNew','apNew2'].forEach(function(id){ document.getElementById(id).value=''; });
      apShowState(true);
      var st=document.getElementById('apState');
      if(st) st.innerHTML='<b style="color:var(--sig)">Password changed.</b> The camera\'s WiFi is restarting. On your phone, open Settings &gt; Wi-Fi and join <b>'+apSsid+'</b> again with the new password, then reload this page.';
      toast('Password changed \u2014 rejoin the WiFi with the new one');
    })
    .catch(function(){ b.disabled=false; b.textContent='SET PASSWORD'; toast('No response',true); });
}
pollStatus();
setInterval(pollStatus,6000);
</script>
</body>
</html>)rawliteral";

// ═══════════════════════════════════════════════════════════════════════
//  QUERY-STRING HELPERS
// ═══════════════════════════════════════════════════════════════════════
static bool qGet(httpd_req_t* req, const char* key, char* out, size_t outLen) {
  char q[192];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
  return httpd_query_key_value(q, key, out, outLen) == ESP_OK;
}
static bool qInt(httpd_req_t* req, const char* key, int* out) {
  char v[16];
  if (!qGet(req, key, v, sizeof(v))) return false;
  *out = atoi(v);
  return true;
}
static esp_err_t sendJson(httpd_req_t* req, const String& j) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, j.c_str(), j.length());
}

// ═══════════════════════════════════════════════════════════════════════
//  HANDLERS — run on the httpd task; blocking camera/SD work is safe here
// ═══════════════════════════════════════════════════════════════════════

static esp_err_t h_index(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  // Without this Safari keeps serving the previous firmware's page from cache,
  // so a reflash appears to change nothing.
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_ui_art(httpd_req_t* req) {
  char skin[16] = "nacre";
  const char* name = strrchr(req->uri, '/');
  if (name && name[1]) {
    name++;
    size_t n = strcspn(name, ".?");
    if (n > 0 && n < sizeof(skin)) {
      memcpy(skin, name, n);
      skin[n] = 0;
    }
  }
  size_t len = 0;
  const uint8_t* art = wuwUiArtForSkin(skin, &len);
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
  return httpd_resp_send(req, reinterpret_cast<const char*>(art), len);
}

// Browser-side diagnostics: the page reports errors here and they land on the
// serial console, which is the only way to see what a phone's browser is doing.
static esp_err_t h_log(httpd_req_t* req) {
  char m[192] = {0};
  if (qGet(req, "m", m, sizeof(m))) { Serial.printf("[PAGE] %s\n", m); ev("PAGE %s", m); }
  return sendJson(req, "{\"ok\":true}");
}

// Single JPEG frame — sent straight from the framebuffer (no heap copy).
/* ── PRE-ROLL ─────────────────────────────────────────────────────────────
 * A camera you press at the right moment is a camera you have already missed
 * the moment with. Between deciding and pressing there is most of a second,
 * and in a room full of people that second is usually the one you wanted.
 *
 * So the last few seconds are always in memory. The ring holds the frames
 * the viewfinder has just shown; pressing SAVE writes out what happened
 * BEFORE the press. The shutter stops being the moment of capture and
 * becomes the moment of choosing.
 *
 * WHERE THE FRAMES COME FROM, EXACTLY
 * Nothing here asks the sensor for anything. The ring is filled from frames
 * the camera was already producing for whoever is looking -- a browser
 * polling /jpg, the MJPEG stream, the panel's viewfinder. That has one
 * honest consequence worth stating plainly: WHEN NOBODY IS LOOKING, THERE IS
 * NO PRE-ROLL. A camera in a pocket with no viewfinder open has nothing in
 * the ring. Buying the alternative -- a task grabbing frames forever --
 * would cost sensor time, current and heat around the clock to serve a
 * feature used seconds a day, so it is not bought.
 *
 * The buffers live in PSRAM, which is the only reason this is affordable:
 * 8 MB sitting almost idle, against 320 KB of internal RAM that is not.
 */
#define PREROLL_MAX_SLOTS  48
#define PREROLL_SLOT_BYTES (72 * 1024)     // ample for SVGA at q10-12
#define PREROLL_PSRAM_FLOOR (1024 * 1024)  // never squeeze the camera or gallery

struct PreSlot { uint8_t* buf; size_t len; uint32_t ms; };
static PreSlot   preRing[PREROLL_MAX_SLOTS];
static int       preSlots   = 0;           // 0 = disarmed / never allocated
static int       preHead    = 0;
static uint32_t  preStored  = 0;           // total frames ever taken in
static bool      preOn      = false;

/* TWO servers feed this ring: /jpg runs on the web httpd task and /stream on
   the MJPEG one. They are separate FreeRTOS tasks, so without a lock two
   frames can interleave into the same slot and what gets written to the card
   is half of one photograph and half of another. A frame is skipped rather
   than queued when the lock is held -- there is another one along in 80 ms,
   and dropping it costs nothing a viewer can see. */
static SemaphoreHandle_t preMux = nullptr;

static void prerollFree() {                // caller must hold preMux
  preOn = false;
  for (int i = 0; i < preSlots; i++) {
    if (preRing[i].buf) heap_caps_free(preRing[i].buf);
    preRing[i].buf = nullptr; preRing[i].len = 0;
  }
  preSlots = 0; preHead = 0;
}

/* Allocates as many slots as PSRAM can spare up to the request, and reports
   what it actually got rather than what was asked for. */
static int prerollArm(int want) {
  if (!preMux) preMux = xSemaphoreCreateMutex();
  if (!preMux) return 0;
  if (xSemaphoreTake(preMux, pdMS_TO_TICKS(500)) != pdTRUE) return preSlots;
  prerollFree();
  if (want <= 0) { xSemaphoreGive(preMux); return 0; }
  if (want > PREROLL_MAX_SLOTS) want = PREROLL_MAX_SLOTS;
  int got = 0;
  for (int i = 0; i < want; i++) {
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
        < PREROLL_SLOT_BYTES + PREROLL_PSRAM_FLOOR) break;
    uint8_t* b = (uint8_t*)heap_caps_malloc(PREROLL_SLOT_BYTES, MALLOC_CAP_SPIRAM);
    if (!b) break;
    preRing[i].buf = b; preRing[i].len = 0; preRing[i].ms = 0;
    got++;
  }
  preSlots = got; preHead = 0; preStored = 0;
  preOn = got > 0;
  xSemaphoreGive(preMux);
  ev("PREROLL %d slots (%u KB)", got, (unsigned)(got * PREROLL_SLOT_BYTES / 1024));
  return got;
}

/* Called with a frame the camera produced for someone else. Copying is the
   whole cost: no decode, no re-encode, no sensor time. A frame too big for a
   slot is skipped rather than truncated -- half a JPEG is not a photograph. */
static inline void prerollFeed(const uint8_t* buf, size_t len) {
  if (!preOn || preSlots <= 0 || len == 0 || len > PREROLL_SLOT_BYTES) return;
  if (!preMux || xSemaphoreTake(preMux, 0) != pdTRUE) return;   // never wait
  if (preOn && preSlots > 0) {                 // re-check: arm may have run
    PreSlot& sl = preRing[preHead];
    memcpy(sl.buf, buf, len);
    sl.len = len;
    sl.ms  = millis();
    preHead = (preHead + 1) % preSlots;
    preStored++;
  }
  xSemaphoreGive(preMux);
}

static esp_err_t h_jpg(httpd_req_t* req) {
  if (!camOk) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera offline");
    return ESP_FAIL;
  }
  /* camFrameAcquire hands back a real frame normally, but during a recording the
     recorder owns the sensor and this gets its latest frame instead -- taking
     one off the sensor here would steal it from the clip. */
  CamFrame fb;
  if (!camFrameAcquire(&fb)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  prerollFeed(fb.buf, fb.len);       // free: this frame already exists
  esp_err_t res = httpd_resp_send(req, (const char*)fb.buf, fb.len);
  camFrameRelease(&fb);
  return res;
}

// Full-resolution photo straight to the browser as a download.
// Same res-switch dance as capturePhoto(), but the bytes go to the client
// instead of the SD card — works with no card inserted.
static esp_err_t h_photo(httpd_req_t* req) {
  if (!camOk) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera offline");
    return ESP_FAIL;
  }
  sensor_t* s = esp_camera_sensor_get();
  bool fullRes = !rec_is_active();   // never touch framesize mid-recording
  framesize_t saved = (framesize_t)streamRes;

  if (fullRes) {
    s->set_quality(s, photoQ);                 // stills deserve the good bitrate
    s->set_framesize(s, (framesize_t)photoRes);
    delay(500);
    // The first frames after a framesize change carry the OLD exposure/white
    // balance. Throw several away so auto-exposure reconverges at full res —
    // this is the single biggest still-quality win.
    for (int i = 0; i < 4; i++) {
      camera_fb_t* j = esp_camera_fb_get();
      if (j) esp_camera_fb_return(j);
      delay(60);
    }
  }

  CamFrame fb;                                  // shared with the recorder if one is running
  esp_err_t res = ESP_FAIL;
  if (camFrameAcquire(&fb)) {
    httpd_resp_set_type(req, "image/jpeg");
    /* inline, not attachment: on a phone an inline image can be long-pressed into Photos, while an
       attachment is filed away in the Files app. */
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=wuwcam.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    const uint8_t turns = camViewRotNow();
    if (jpegNeedsOrientation(fb.buf, fb.len, turns)) {       // same tag the SD photos carry
      uint8_t seg[JPEG_EXIF_BYTES];
      size_t segN = jpegExifSegment(jpegOrientationForTurns(turns), seg);
      res = httpd_resp_send_chunk(req, (const char*)fb.buf, 2);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)seg, segN);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb.buf + 2, fb.len - 2);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, nullptr, 0);
    } else {
      res = httpd_resp_send(req, (const char*)fb.buf, fb.len);
    }
    Serial.printf("[PHOTO] %ux%u %uKB\n", (unsigned)fb.w, (unsigned)fb.h, (unsigned)(fb.len / 1024));
    camFrameRelease(&fb);
  } else {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame");
  }

  if (fullRes) {
    s->set_framesize(s, saved);
    s->set_quality(s, streamQ);
  }
  return res;
}

/* iOS cannot save browser-generated blobs over plain HTTP: <a download> is
   ignored and navigator.share needs HTTPS. But Safari DOES honour a server
   Content-Disposition. So the page posts the finished recording here, we park
   it in PSRAM, and /dl hands it back as a genuine file download. */
#define UP_MAX (2 * 1024 * 1024)
static uint8_t* upBuf  = NULL;
static size_t   upLen  = 0;
static char     upMime[32] = "video/mp4";
static char     upName[48] = "WUWCAM.mp4";
/* One PSRAM stash is shared by every viewer, and esp_http_server serves one
   request at a time -- so there is no data race, but there IS a race across
   requests: phone A uploads, phone B uploads (overwriting), then A downloads
   and gets B's file under A's filename. The token closes that. A mismatch is
   refused rather than served, because handing someone another person's photo
   is worse than making them press save twice. */
static uint32_t upTok = 0;
static volatile bool upServing = false;     // a download of the stash is in flight: do not free it underneath

static void sanitize(char* d, size_t dn, const char* srcs) {
  size_t j = 0;
  for (size_t i = 0; srcs[i] && j < dn - 1; i++) {
    char ch = srcs[i];
    bool ok = (ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||
              ch=='.'||ch=='_'||ch=='-';
    if (ok) d[j++] = ch;
  }
  d[j] = 0;
  if (!j) snprintf(d, dn, "WUWCAM.bin");
}

static esp_err_t h_upload(httpd_req_t* req) {
  /* The 2 MB stash is given back while a clip records (recModeEnter); taking it again now would
     cut straight into the recorder's ring. The page waits until the recording has stopped. */
  if (rec_is_active())
    return sendJson(req, "{\"ok\":false,\"err\":\"recording - save after it stops\"}");
  size_t total = req->content_len;
  if (!total || total > UP_MAX)
    return sendJson(req, "{\"ok\":false,\"err\":\"size\"}");

  if (!upBuf) {
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < UP_MAX + 256 * 1024)
      return sendJson(req, "{\"ok\":false,\"err\":\"low PSRAM\"}");
    upBuf = (uint8_t*)heap_caps_malloc(UP_MAX, MALLOC_CAP_SPIRAM);
    if (!upBuf) return sendJson(req, "{\"ok\":false,\"err\":\"psram\"}");
  }

  char q[96];
  if (qGet(req, "name", q, sizeof(q))) sanitize(upName, sizeof(upName), q);
  if (qGet(req, "mime", q, sizeof(q))) {
    // query strings cannot carry '/', so the page sends "video-mp4"
    for (size_t i = 0; q[i]; i++) if (q[i] == '-') { q[i] = '/'; break; }
    /* q holds up to 95 bytes and upMime is 32: bound the copy explicitly so
       the truncation is a stated decision rather than something the compiler
       has to warn about. Real MIME types are far shorter than this. */
    snprintf(upMime, sizeof(upMime), "%.31s", q);
  }

  size_t got = 0;
  while (got < total) {
    int r = httpd_req_recv(req, (char*)upBuf + got, total - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) { upLen = 0; return ESP_FAIL; }
    got += r;
  }
  upLen = got;
  upTok = esp_random() | 1u;                 // never 0, so 0 means "no stash"
  Serial.printf("[UP] %s %s %u bytes tok=%u\n", upName, upMime,
                (unsigned)upLen, (unsigned)upTok);
  return sendJson(req, "{\"ok\":true,\"len\":" + String((uint32_t)upLen) +
                       ",\"tok\":" + String(upTok) + "}");
}

/* Range handling shared by everything that serves a video. Reads the request's Range header and
   reports it through parseByteRangeHeader(); sendPartial() answers with a 206 whose Content-Length
   is exact (esp_http_server sets it from the buffer), which is what Safari insists on. */
static RangeParse requestRange(httpd_req_t* req, size_t total, size_t* first, size_t* last) {
  char h[64];
  size_t hl = httpd_req_get_hdr_value_len(req, "Range");
  if (!hl || hl >= sizeof(h)) return RANGE_NONE;
  if (httpd_req_get_hdr_value_str(req, "Range", h, sizeof(h)) != ESP_OK) return RANGE_NONE;
  return parseByteRangeHeader(h, total, first, last);
}
static esp_err_t sendRangeRefused(httpd_req_t* req, size_t total) {
  char cr[40];
  snprintf(cr, sizeof(cr), "bytes */%u", (unsigned)total);
  httpd_resp_set_status(req, "416 Range Not Satisfiable");
  httpd_resp_set_hdr(req, "Content-Range", cr);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, "", 0);
}
static esp_err_t sendPartial(httpd_req_t* req, const uint8_t* data, size_t first, size_t len, size_t total) {
  char cr[56];
  snprintf(cr, sizeof(cr), "bytes %u-%u/%u", (unsigned)first, (unsigned)(first + len - 1), (unsigned)total);
  httpd_resp_set_status(req, "206 Partial Content");
  httpd_resp_set_hdr(req, "Content-Range", cr);
  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  return httpd_resp_send(req, (const char*)data, len);
}

static esp_err_t h_dl(httpd_req_t* req) {
  if (!upBuf || !upLen) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "nothing stored");
    return ESP_FAIL;
  }
  char kbuf[16];
  uint32_t want = 0;
  if (qGet(req, "k", kbuf, sizeof(kbuf))) want = (uint32_t)strtoul(kbuf, nullptr, 10);
  if (!upTok || want != upTok) {
    ev("DL refused: token %u != %u", (unsigned)want, (unsigned)upTok);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req,
      "another device replaced the stashed file - press save again",
      HTTPD_RESP_USE_STRLEN);
  }
  /* inline=1 is for phones: Safari opens an inline video in its own player, whose share sheet has
     "Save Video" (into Photos), and an inline image can be long-pressed into Photos. An attachment
     is filed in the Files app, which is exactly what we are avoiding. */
  char flag[4];
  const bool inlineView = qGet(req, "inline", flag, sizeof(flag)) && flag[0] == '1';
  char cd[128];
  snprintf(cd, sizeof(cd), "%s; filename=\"%s\"", inlineView ? "inline" : "attachment", upName);
  httpd_resp_set_type(req, upMime);
  httpd_resp_set_hdr(req, "Content-Disposition", cd);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  size_t first = 0, last = 0;
  RangeParse rp = requestRange(req, upLen, &first, &last);
  if (rp == RANGE_BAD) return sendRangeRefused(req, upLen);
  upServing = true;
  esp_err_t sent = (rp == RANGE_OK)
      ? sendPartial(req, upBuf + first, first, last - first + 1, upLen)
      : httpd_resp_send(req, (const char*)upBuf, upLen);
  upServing = false;
  return sent;
}

/* Connectivity probes.
   iOS/Android/Windows each fetch a known URL after joining a network. If the
   answer is not what they expect, the OS opens a captive-login WebView — and on
   iOS that "Captive WLAN" browser CANNOT download files or save to Photos,
   which breaks the whole point of the camera. So we answer each probe with
   exactly what it wants ("there is internet here"), the OS stays out of the
   way, and the user opens the app in real Safari where saving works. */
static esp_err_t h_probe(httpd_req_t* req) {
  const char* u = req->uri;
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (strstr(u, "generate_204") || strstr(u, "gen_204")) {   // Android
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, "", 0);
  }
  if (strstr(u, "connecttest") || strstr(u, "ncsi.txt")) {   // Windows
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "Microsoft Connect Test", HTTPD_RESP_USE_STRLEN);
  }
  // Apple expects this exact body
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req,
    "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>",
    HTTPD_RESP_USE_STRLEN);
}

/* Anything else unknown still lands on the app, so typing any address works. */
static esp_err_t h_portal(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, "", 0);
}

static esp_err_t h_info(httpd_req_t* req) {
  uint8_t m[6]; WiFi.softAPmacAddress(m);
  char id[16]; snprintf(id, sizeof(id), "%02X%02X%02X", m[3], m[4], m[5]);
  String j = String("{\"model\":\"") + FW_MODEL + "\",\"fw\":\"" + FW_VERSION +
             "\",\"serial\":\"" + id + "\",\"ssid\":\"" + apName +
             "\",\"psram\":" + String(ESP.getFreePsram()) +
             ",\"heap\":" + String(ESP.getFreeHeap()) +
             ",\"cam\":" + String(camOk ? "true" : "false") + "}";
  return sendJson(req, j);
}

/* ── SHARED SESSION ─────────────────────────────────────────────────────
   Everyone on the AP sees the same viewfinder, anyone can fire the shutter,
   and every shot lands in a gallery held in PSRAM that all viewers can pull
   from. Because the look-up (GPU FX) runs on each viewer's own device, five
   people can take home five different renderings of the same instant — that
   is the thing a normal camera cannot do.

   Fixed-size slots, no fragmentation, oldest shot is overwritten. */
#define GAL_SLOTS    8
#define GAL_SLOT_SZ  (140 * 1024)
#define CLI_MAX      8
#define CLI_TIMEOUT  25000UL

static uint8_t* galArena = NULL;
static struct { uint32_t id, ms, len; char by[18]; } galMeta[GAL_SLOTS];
static int      galHead   = 0;
static uint32_t galNextId = 1;
static uint32_t galGen    = 0;      // bumps on any change; viewers poll this
static struct { char name[18]; uint32_t seen; bool used; } clients[CLI_MAX];

static bool galInit() {
  if (galArena) return true;
  size_t need = (size_t)GAL_SLOTS * GAL_SLOT_SZ;
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < need + 256 * 1024) {
    Serial.println("[GAL] not enough PSRAM headroom");
    return false;
  }
  galArena = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
  if (!galArena) { Serial.println("[GAL] PSRAM alloc failed"); return false; }
  memset(galMeta, 0, sizeof(galMeta));
  Serial.printf("[GAL] %d slots x %d KB\n", GAL_SLOTS, GAL_SLOT_SZ / 1024);
  return true;
}

// Names land in JSON and in the UI, so keep them to a safe alphabet.
static void cleanName(char* d, size_t dn, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dn - 1; i++) {
    char c = src[i];
    bool ok = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
              c=='_'||c=='-'||c==' '||c=='.';
    if (ok) d[j++] = c;
  }
  d[j] = 0;
  if (!j) snprintf(d, dn, "guest");
}

static void touchClient(const char* name) {
  uint32_t now = millis();
  int freeSlot = -1;
  for (int i = 0; i < CLI_MAX; i++) {
    if (clients[i].used && (now - clients[i].seen) > CLI_TIMEOUT) clients[i].used = false;
    if (clients[i].used && strcmp(clients[i].name, name) == 0) { clients[i].seen = now; return; }
    if (!clients[i].used && freeSlot < 0) freeSlot = i;
  }
  if (freeSlot >= 0) {
    clients[freeSlot].used = true;
    clients[freeSlot].seen = now;
    snprintf(clients[freeSlot].name, sizeof(clients[freeSlot].name), "%s", name);
    galGen++;                       // presence change is worth a refresh too
  }
}

static String presenceJson() {
  uint32_t now = millis();
  String a = "[";
  bool first = true;
  for (int i = 0; i < CLI_MAX; i++) {
    if (!clients[i].used) continue;
    if ((now - clients[i].seen) > CLI_TIMEOUT) { clients[i].used = false; continue; }
    if (!first) a += ",";
    a += "\"" + String(clients[i].name) + "\"";
    first = false;
  }
  return a + "]";
}

// Lightweight heartbeat: register presence, learn whether anything changed.
static esp_err_t h_sync(httpd_req_t* req) {
  char raw[24] = {0}, nm[18];
  if (qGet(req, "name", raw, sizeof(raw))) { cleanName(nm, sizeof(nm), raw); touchClient(nm); }
  String j = "{\"gen\":" + String(galGen) +
             ",\"who\":" + presenceJson() +
             ",\"cam\":" + String(camOk ? "true" : "false") + "}";
  return sendJson(req, j);
}

// Fire the shared shutter. Deliberately captures at the CURRENT stream size,
// not 5MP: everyone is waiting on it, and a resolution switch stalls the feed
// for ~600 ms for every viewer at once.
static esp_err_t h_shot(httpd_req_t* req) {
  if (!camOk)    return sendJson(req, "{\"ok\":false,\"err\":\"camera offline\"}");
  if (!galInit())return sendJson(req, "{\"ok\":false,\"err\":\"no PSRAM\"}");

  char raw[24] = {0}, by[18];
  cleanName(by, sizeof(by), qGet(req, "by", raw, sizeof(raw)) ? raw : "guest");

  CamFrame fb;                                  // never steal a frame from a running clip
  if (!camFrameAcquire(&fb)) return sendJson(req, "{\"ok\":false,\"err\":\"no frame\"}");
  if (fb.len + JPEG_EXIF_BYTES > GAL_SLOT_SZ) {      // leaves room for the orientation tag
    uint32_t kb = fb.len / 1024;
    camFrameRelease(&fb);
    return sendJson(req, "{\"ok\":false,\"err\":\"frame " + String(kb) +
                          "KB exceeds slot - lower resolution or quality\"}");
  }
  int slot = galHead;
  /* The frame is stored with the orientation it has RIGHT NOW (EXIF), because /frame is cached for a
     year and the VIEW ROTATE setting may change later. */
  MemSink sink = { galArena + (size_t)slot * GAL_SLOT_SZ, GAL_SLOT_SZ, 0 };
  size_t stored = jpegWriteOriented(memSink, &sink, fb.buf, fb.len, camViewRotNow());
  if (!stored) { camFrameRelease(&fb); return sendJson(req, "{\"ok\":false,\"err\":\"slot write failed\"}"); }
  galMeta[slot].id  = galNextId++;
  galMeta[slot].ms  = millis();
  galMeta[slot].len = (uint32_t)stored;
  snprintf(galMeta[slot].by, sizeof(galMeta[slot].by), "%s", by);
  uint32_t id = galMeta[slot].id, len = fb.len;
  camFrameRelease(&fb);

  galHead = (galHead + 1) % GAL_SLOTS;
  galGen++;
  Serial.printf("[SHOT] #%lu by %s (%lu KB)\n", (unsigned long)id, by,
                (unsigned long)(len / 1024));
  return sendJson(req, "{\"ok\":true,\"id\":" + String(id) + "}");
}

// Newest first, so the filmstrip reads left-to-right as most-recent-first.
static esp_err_t h_gallery(httpd_req_t* req) {
  uint32_t now = millis();
  String j = "{\"gen\":" + String(galGen) + ",\"shots\":[";
  bool first = true;
  for (int n = 1; n <= GAL_SLOTS; n++) {
    int i = (galHead - n + GAL_SLOTS * 2) % GAL_SLOTS;
    if (!galMeta[i].len) continue;
    if (!first) j += ",";
    j += "{\"id\":"   + String(galMeta[i].id) +
         ",\"by\":\"" + String(galMeta[i].by) + "\"" +
         ",\"age\":"  + String((now - galMeta[i].ms) / 1000) +
         ",\"kb\":"   + String(galMeta[i].len / 1024) + "}";
    first = false;
  }
  return sendJson(req, j + "]}");
}

// A shot's bytes never change once written, so let browsers cache it forever —
// otherwise every filmstrip refresh re-downloads the whole gallery.
static esp_err_t h_frame(httpd_req_t* req) {
  int want = -1;
  if (!qInt(req, "id", &want) || !galArena) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such shot");
    return ESP_FAIL;
  }
  for (int i = 0; i < GAL_SLOTS; i++) {
    if (galMeta[i].len && (int)galMeta[i].id == want) {
      httpd_resp_set_type(req, "image/jpeg");
      httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
      httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
      return httpd_resp_send(req, (const char*)(galArena + (size_t)i * GAL_SLOT_SZ),
                             galMeta[i].len);
    }
  }
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "expired");
  return ESP_FAIL;
}

/* Card contents over WiFi. Without this, everything the camera records is
   trapped until you physically pull the card — which defeats leaving it
   somewhere. Listing is cheap and lives on :80; the download blocks its server
   for the length of the transfer, so it runs on :81 where it cannot stall the
   UI or the shared session. */
static bool sdSafeMediaPath(const char* path, const char** kind = nullptr) {
  if (!path || !*path || strstr(path, "..")) return false;
  for (const char* p = path; *p; ++p) {
    char c = *p;
    bool ok = isalnum((unsigned char)c) || c == '/' || c == '_' || c == '-' || c == '.';
    if (!ok) return false;
  }
  size_t L = strlen(path);
  bool jpg = L > 4 && !strcasecmp(path + L - 4, ".jpg");
  bool vid = L > 4 && (!strcasecmp(path + L - 4, ".mov") ||
                       !strcasecmp(path + L - 4, ".mp4"));
  if (kind) *kind = jpg ? "jpg" : vid ? "video" : "";
  return jpg || vid;
}

static void appendSdFiles(String& j, const char* dirPath, bool& first, int& count) {
  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File f;
  while (count < 72 && (f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      const char* raw = f.name();
      while (*raw == '/') raw++;
      char rel[112];
      if (strcmp(dirPath, "/") && !strchr(raw, '/')) {
        const char* d = dirPath;
        while (*d == '/') d++;
        snprintf(rel, sizeof(rel), "%s/%s", d, raw);
      } else {
        snprintf(rel, sizeof(rel), "%s", raw);
      }
      const char* kind = nullptr;
      if (!strstr(rel, "_view.jpg") && sdSafeMediaPath(rel, &kind)) {
        if (!first) j += ',';
        j += "{\"n\":\"" + String(rel) + "\",\"kb\":" +
             String((uint32_t)(f.size() / 1024)) + ",\"kind\":\"" + kind + "\"";
#ifdef EXHIBITION_MODE
        if (strcmp(dirPath, "/") && !strcmp(kind, "jpg")) {
          const char* leaf = strrchr(rel, '/');
          leaf = leaf ? leaf + 1 : rel;
          int imageNo = -1;
          if (sscanf(leaf, "IMG_%d", &imageNo) == 1 && imageNo >= 0)
            j += ",\"i\":" + String(imageNo);
        }
#endif
        j += "}";
        first = false;
        count++;
      }
    }
    f.close();
  }
  dir.close();
}

static esp_err_t h_files(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "{\"ok\":false,\"err\":\"no card\"}");
  String j = "{\"ok\":true,\"files\":[";
  bool first = true;
  int n = 0;
#ifdef EXHIBITION_MODE
  /* Put the active roll first. Captures live here, while videos live at root. */
  appendSdFiles(j, pasarSessionDir(), first, n);
#endif
  appendSdFiles(j, "/", first, n);
  return sendJson(req, j + "]}");
}

static esp_err_t h_sdfile(httpd_req_t* req) {
  if (!sdReady) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no card"); return ESP_FAIL; }
  char raw[160] = {0};
  if (!qGet(req, "f", raw, sizeof(raw))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no name"); return ESP_FAIL;
  }
  char decoded[128];
  urlDecode(raw, decoded, sizeof(decoded));
  const char* name = decoded;
  while (*name == '/') name++;
  const char* kind = nullptr;
  if (!sdSafeMediaPath(name, &kind)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path"); return ESP_FAIL;
  }
  String path = "/" + String(name);
  File f = SD_MMC.open(path.c_str(), FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found"); return ESP_FAIL;
  }
  size_t L = strlen(name);
  const char* ext = (L > 4) ? name + L - 4 : "";
  const char* ctype = !strcasecmp(ext, ".mov") ? "video/quicktime"
                    : !strcasecmp(ext, ".mp4") ? "video/mp4" : "image/jpeg";
  httpd_resp_set_type(req, ctype);
  /* Everything is served inline by default, videos included. An attachment goes to the Files app on
     an iPhone; an inline video opens in Safari's player, whose share sheet saves it to Photos. The
     explicit Save button on Android and desktop asks for dl=1 and gets a real download. */
  char flag[4];
  const bool download = qGet(req, "dl", flag, sizeof(flag)) && flag[0] == '1';
  char cd[160];
  const char* leaf = strrchr(name, '/');
  leaf = leaf ? leaf + 1 : name;
  snprintf(cd, sizeof(cd), "%s; filename=\"%s\"", download ? "attachment" : "inline", leaf);
  httpd_resp_set_hdr(req, "Content-Disposition", cd);
  httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");

  const size_t fileSize = f.size();
  size_t first = 0, last = 0;
  RangeParse rp = requestRange(req, fileSize, &first, &last);
  if (rp == RANGE_BAD) { f.close(); return sendRangeRefused(req, fileSize); }
  if (rp == RANGE_OK) {
    /* Serve a bounded slice and say exactly what it is: a client may be sent less than it asked
       for and simply continues from where the slice ended. Bounding it keeps a 200 MB clip from
       needing 200 MB of RAM. */
    const size_t SLICE = 192 * 1024;
    size_t want = last - first + 1;
    if (want > SLICE) want = SLICE;
    uint8_t* part = (uint8_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
    if (!part) { want = 32 * 1024 < want ? 32 * 1024 : want; part = (uint8_t*)malloc(want); }
    if (!part) { f.close(); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem"); return ESP_FAIL; }
    esp_err_t pr = ESP_FAIL;
    if (f.seek(first)) {
      size_t got = f.read(part, want);
      if (got) pr = sendPartial(req, part, first, got, fileSize);
    }
    if (pr != ESP_OK) httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read");
    free(part);                       // heap_caps_malloc memory is released by free() as well
    f.close();
    return pr;
  }

  uint8_t* buf = (uint8_t*)malloc(4096);
  if (!buf) { f.close(); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mem"); return ESP_FAIL; }
  esp_err_t r = ESP_OK;
  size_t total = 0;
  while (true) {
    int n = f.read(buf, 4096);
    if (n <= 0) break;
    r = httpd_resp_send_chunk(req, (const char*)buf, n);
    if (r != ESP_OK) break;
    total += n;
  }
  free(buf);
  f.close();
  if (r == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
  Serial.printf("[SD] served %s (%u KB)\n", name, (unsigned)(total / 1024));
  return r;
}

static size_t jsonQuoted(char* out, size_t cap, size_t at, const char* text);

static esp_err_t h_net(httpd_req_t* req) {
  char raw[128], v[96];
  bool changed = false;
  if (qGet(req, "ssid", raw, sizeof(raw))) {
    urlDecode(raw, v, sizeof(v)); staSsid = v; prefs.putString("ssid", staSsid); changed = true;
  }
  if (qGet(req, "pass", raw, sizeof(raw))) {
    urlDecode(raw, v, sizeof(v)); staPass = v; prefs.putString("pass", staPass); changed = true;
  }
  if (qGet(req, "url", raw, sizeof(raw))) {
    urlDecode(raw, v, sizeof(v)); otaUrl = v; prefs.putString("otaurl", otaUrl);
  }
  if (changed && staSsid.length()) {
    /* Do not hold the only web handler in a 12-second association loop. The
       browser polls this endpoint while the WiFi task connects in parallel. */
    staJoin(staSsid.c_str(), staPass.c_str());
  } else {
    staOn = WiFi.status() == WL_CONNECTED;
  }
  static char out[320];
  size_t at = snprintf(out, sizeof(out), "{\"ok\":true,\"ssid\":");
  at = jsonQuoted(out, sizeof(out), at, staSsid.c_str());
  at += snprintf(out + at, sizeof(out) - at, ",\"online\":%s,\"ip\":",
                 staOn ? "true" : "false");
  String ip = staOn ? WiFi.localIP().toString() : String("");
  at = jsonQuoted(out, sizeof(out), at, ip.c_str());
  at += snprintf(out + at, sizeof(out) - at, ",\"connecting\":%s}",
                 !staOn && staSsid.length() ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out, at);
}

static size_t jsonQuoted(char* out, size_t cap, size_t at, const char* text) {
  if (at + 1 < cap) out[at++] = '"';
  for (size_t i = 0; text && text[i] && at + 7 < cap; ++i) {
    unsigned char c = text[i];
    if (c == '"' || c == '\\') { out[at++] = '\\'; out[at++] = c; }
    else if (c >= 0x20) out[at++] = c;
  }
  if (at + 1 < cap) out[at++] = '"';
  return at;
}

static esp_err_t h_net_scan(httpd_req_t* req) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    WiFi.scanNetworks(true, true);
    return sendJson(req, "{\"ok\":true,\"scanning\":true}");
  }
  if (n == WIFI_SCAN_RUNNING)
    return sendJson(req, "{\"ok\":true,\"scanning\":true}");

  static char out[3072];
  size_t at = snprintf(out, sizeof(out), "{\"ok\":true,\"scanning\":false,\"nets\":[");
  int shown = min(n, 24);
  for (int i = 0; i < shown && at + 96 < sizeof(out); ++i) {
    if (i) out[at++] = ',';
    at += snprintf(out + at, sizeof(out) - at, "{\"ssid\":");
    at = jsonQuoted(out, sizeof(out), at, WiFi.SSID(i).c_str());
    at += snprintf(out + at, sizeof(out) - at, ",\"rssi\":%ld,\"secure\":%s}",
                   (long)WiFi.RSSI(i),
                   WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true");
  }
  at += snprintf(out + at, sizeof(out) - at, "]}");
  WiFi.scanDelete();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, out, at);
}

/* Check only — never downloads. Compares the manifest's version against the
   build actually running, so the user sees what they would be getting. */
static esp_err_t h_ota_check(httpd_req_t* req) {
  if (!staOn && !staConnect())
    return sendJson(req, "{\"ok\":false,\"err\":\"not connected to the internet\"}");
  WiFiClientSecure sec; sec.setInsecure();
  WiFiClient plain;
  HTTPClient http;
  bool https = otaUrl.startsWith("https");
  bool began = https ? http.begin(sec, otaUrl) : http.begin(plain, otaUrl);
  if (!began) return sendJson(req, "{\"ok\":false,\"err\":\"bad update URL\"}");
  int code = http.GET();
  if (code != 200) { http.end();
    return sendJson(req, "{\"ok\":false,\"err\":\"manifest HTTP " + String(code) + "\"}"); }
  String body = http.getString(); http.end();

  auto field = [&](const char* k) -> String {
    int i = body.indexOf(String("\"") + k + "\"");
    if (i < 0) return "";
    int c = body.indexOf(':', i); if (c < 0) return "";
    int a = body.indexOf('"', c); if (a < 0) return "";
    int b = body.indexOf('"', a + 1); if (b < 0) return "";
    return body.substring(a + 1, b);
  };
  otaLatest = field("version");
  otaNotes  = field("notes");
  bool avail = otaLatest.length() && otaLatest != String(FW_VERSION);
  ev("OTA check: running %s, offered %s", FW_VERSION, otaLatest.c_str());
  return sendJson(req, "{\"ok\":true,\"current\":\"" + String(FW_VERSION) +
                       "\",\"latest\":\"" + otaLatest +
                       "\",\"update\":" + String(avail ? "true" : "false") +
                       ",\"notes\":\"" + otaNotes + "\"}");
}

static esp_err_t h_ota_apply(httpd_req_t* req) {
  if (rec_is_active())
    return sendJson(req, "{\"ok\":false,\"err\":\"stop recording first\"}");
  if (!staOn && !staConnect())
    return sendJson(req, "{\"ok\":false,\"err\":\"not connected to the internet\"}");

  WiFiClientSecure sec; sec.setInsecure();
  WiFiClient plain;
  HTTPClient http;
  bool https = otaUrl.startsWith("https");
  if (!(https ? http.begin(sec, otaUrl) : http.begin(plain, otaUrl)))
    return sendJson(req, "{\"ok\":false,\"err\":\"bad update URL\"}");
  if (http.GET() != 200) { http.end();
    return sendJson(req, "{\"ok\":false,\"err\":\"manifest unreachable\"}"); }
  String body = http.getString(); http.end();
  int a = body.indexOf("\"url\"");
  int q1 = a < 0 ? -1 : body.indexOf('"', body.indexOf(':', a));
  int q2 = q1 < 0 ? -1 : body.indexOf('"', q1 + 1);
  if (q2 < 0) return sendJson(req, "{\"ok\":false,\"err\":\"manifest has no firmware url\"}");
  String bin = body.substring(q1 + 1, q2);

  ev("OTA downloading %s", bin.c_str());
  // Answer BEFORE flashing: the write takes ~30 s and reboots on success, so
  // the page must already know to wait rather than sit on a dead socket.
  sendJson(req, "{\"ok\":true,\"msg\":\"downloading — the camera will reboot\"}");
  delay(250);

  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return r = https ? httpUpdate.update(sec, bin)
                                : httpUpdate.update(plain, bin);
  // only reached when the update did NOT happen; the old firmware is intact
  ev("OTA failed: %s", httpUpdate.getLastErrorString().c_str());
  Serial.printf("[OTA] failed (%d) %s\n", (int)r, httpUpdate.getLastErrorString().c_str());
  return ESP_OK;
}

static esp_err_t h_status(httpd_req_t* req) {
  sensor_t* sensor = camOk ? esp_camera_sensor_get() : nullptr;
  String j = "{\"night\":"  + String(nightVision ? "true" : "false") +
             ",\"sd\":"     + String(sdReady     ? "true" : "false") +
             ",\"photos\":" + String(photoCount) +
             ",\"res\":"    + String(streamRes) +
             ",\"cam\":"    + String(camOk       ? "true" : "false") +
             ",\"sensor\":\"" + String(camSensorName) + "\"" +
             ",\"sensorPid\":" + String(camSensorPid) +
             ",\"maxRes\":" + String(camMaxRes) +
             ",\"build\":\"" + String(__DATE__ " " __TIME__) + "\"" +
             ",\"heap\":"   + String(ESP.getFreeHeap()) +
             ",\"rec\":"    + String(rec_is_active() ? "true" : "false") +
             ",\"photoRes\":" + String(photoRes) +
             ",\"streamQ\":" + String(streamQ) +
             ",\"photoQ\":" + String(photoQ) +
             ",\"bright\":" + String(camBrightness) +
             ",\"contrast\":" + String(sensor ? sensor->status.contrast : 0) +
             ",\"aeLevel\":" + String(sensor ? sensor->status.ae_level : 0) +
             ",\"hmirror\":" + String(camHMirror ? "true" : "false") +
             ",\"vflip\":" + String(camVFlip ? "true" : "false") +
             ",\"viewrot\":" + String(camViewRotNow()) +
             ",\"apCustom\":" + String(apPassCustom ? "true" : "false") +
             ",\"sat\":" + String(sensor ? sensor->status.saturation : 0) +
             ",\"wb\":" + String(prefs.getInt("wbmode", 0)) +
             ",\"lag\":"   + String(lastShutterLagMs) +
             ",\"peers\":" + String(linkPeerCount()) + "}";
  return sendJson(req, j);
}

/* GET /ap -> whether the owner has set their own WiFi password. The password itself is never sent.
   POST /ap/pass with the body "<current password>\n<new password>" changes it. The page is
   told first and the radio restarts about a second and a half later (apTick), because applying it
   drops every phone, including the one that asked.
   Two guards beyond the password rules: the request must carry a custom header, which a web page
   on another site cannot add without a CORS preflight this server never grants (so a page the owner
   merely visits cannot change the password), and the CURRENT password must be right, so a guest
   who was given the WiFi cannot lock the owner out. */
static esp_err_t h_ap(httpd_req_t* req) {
  return sendJson(req, String("{\"ok\":true,\"custom\":") + (apPassCustom ? "true" : "false") +
                       ",\"ssid\":\"" + apName + "\",\"min\":" + String(AP_PASS_MIN) +
                       ",\"max\":" + String(AP_PASS_MAX) + "}");
}

static esp_err_t h_ap_pass(httpd_req_t* req) {
  if (httpd_req_get_hdr_value_len(req, "X-Wuw-Ap") == 0)
    return sendJson(req, "{\"ok\":false,\"err\":\"not allowed\"}");
  size_t n = req->content_len;
  if (n < 3 || n > 2 * AP_PASS_MAX + 2)
    return sendJson(req, "{\"ok\":false,\"err\":\"bad request\"}");
  char body[2 * AP_PASS_MAX + 3];
  size_t got = 0;
  for (int stalls = 0; got < n && stalls < 40;) {
    int r = httpd_req_recv(req, body + got, n - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) { ++stalls; continue; }
    if (r <= 0) return ESP_FAIL;
    got += (size_t)r;
  }
  if (got != n) return ESP_FAIL;
  body[got] = 0;
  char* nl = strchr(body, '\n');
  if (!nl) return sendJson(req, "{\"ok\":false,\"err\":\"bad request\"}");
  *nl = 0;
  const char* current = body;
  const char* wanted  = nl + 1;
  size_t curLen = strlen(current);

  /* constant-time compare, and a pause on a miss so guessing is slow */
  unsigned diff = (unsigned)(curLen ^ strlen(apPass));
  for (size_t i = 0; i < curLen && i < sizeof(apPass); ++i) diff |= (unsigned)(current[i] ^ apPass[i]);
  if (diff) {
    vTaskDelay(pdMS_TO_TICKS(700));
    ev("AP password change refused: current password wrong");
    return sendJson(req, "{\"ok\":false,\"err\":\"current password is wrong\"}");
  }
  ApPassCheck c = apPassCheck(wanted, strlen(wanted));
  if (c != AP_PASS_OK)
    return sendJson(req, String("{\"ok\":false,\"err\":\"") + apPassCheckText(c) + "\"}");
  if (!strcmp(wanted, apPass))
    return sendJson(req, "{\"ok\":false,\"err\":\"that is already the password\"}");

  prefs.putString("appass", wanted);
  snprintf(apPass, sizeof(apPass), "%s", wanted);
  apPassCustom = true;
  apApplyAt = (millis() + 1500) | 1u;
  ev("AP password changed - restarting the access point");
  memset(body, 0, sizeof(body));
  return sendJson(req, "{\"ok\":true,\"custom\":true,\"applyMs\":1500}");
}

static uint32_t lastShutterMs = 0;
#define SHUTTER_MIN_GAP 900          // ms; a 5MP capture already takes ~600


/* ── The shutter, serialised ──────────────────────────────────────────────
 * capturePhoto() switches the sensor's frame size, waits for it to settle,
 * grabs, writes to SD and switches back. It is not reentrant, and it had TWO
 * callers in different tasks: the web handler on the httpd task (priority 5)
 * and the hardware button drained from loop() (priority 1). httpd preempts
 * loop, so a button capture could be interrupted mid-sequence by a web one --
 * corrupting the sensor state and double-counting the file number.
 *
 * Everything now goes through here: one mutex, one rate limit, one place that
 * knows what happens after a successful frame. The mutex is taken with a
 * timeout rather than forever, so a wedged capture refuses the next request
 * instead of deadlocking the web server.
 */
static SemaphoreHandle_t shutterMux = nullptr;
struct ShotResult { bool ok; bool busy; String detail; uint32_t num; };

static ShotResult shutterTake(const char* who) {
  ShotResult r = { false, false, "", 0 };
  if (!shutterMux) { r.detail = "ERR_NO_MUTEX"; return r; }
  if (xSemaphoreTake(shutterMux, pdMS_TO_TICKS(50)) != pdTRUE) {
    r.busy = true; r.detail = "BUSY"; return r;      // someone else has it
  }
  uint32_t nowMs = millis();
  if (lastShutterMs && (nowMs - lastShutterMs) < SHUTTER_MIN_GAP) {
    xSemaphoreGive(shutterMux);
    r.busy = true; r.detail = "BUSY"; return r;
  }
  lastShutterMs = nowMs;

  if (!camOk)          { xSemaphoreGive(shutterMux); r.detail = "ERR_NO_CAMERA"; return r; }
  if (rec_is_active()) { xSemaphoreGive(shutterMux); r.detail = "ERR_RECORDING"; return r; }

  String d = capturePhoto();
  r.detail = d;
  r.ok = (d.indexOf("ERR") < 0);
  r.num = (uint32_t)photoCount - 1;
  xSemaphoreGive(shutterMux);

  ev("CAPTURE -> %s", d.c_str());
#ifdef EXHIBITION_MODE
  if (r.ok) {
    pasarShutter(r.num, who);
    wuwBridgeShutter(r.num, who);
    wuwBridgePhotoSaved(r.num);
    /* No chat line here. The camera does not narrate itself into the room --
       a message is posted only when a person deliberately writes one. */
  }
#endif
  return r;
}

/* ── Recording comes first ────────────────────────────────────────────────
 * A smooth high-frame-rate clip needs two things the rest of the camera competes for: PSRAM (the
 * recorder's ring of frames is the margin that absorbs a slow SD write, and it is sized from
 * whatever PSRAM is free) and memory bandwidth. So when a clip starts, everything that is not
 * needed while it runs is given back, and put back when it ends:
 *   - the stash for browser-made files (2 MB), which nobody uses mid-recording,
 *   - the pre-roll ring (up to 3.4 MB) and the frame copies it makes for every web viewer.
 * The recorder's own idle buffers (effect ring, preview slots) are released at rec_stop, and the
 * TFT preview drops its colour effects and slows down while a clip runs (panel.cpp).
 * The numbers are logged so the gain is measured, not assumed. */
static bool recModeOn = false;
static int  recModePrerollSlots = 0;

static void recModeEnter() {
  const uint32_t before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (upBuf && !upServing) { heap_caps_free(upBuf); upBuf = nullptr; upLen = 0; upTok = 0; }
  recModePrerollSlots = 0;
  if (preMux && xSemaphoreTake(preMux, pdMS_TO_TICKS(500)) == pdTRUE) {
    if (preOn) recModePrerollSlots = preSlots;
    prerollFree();
    xSemaphoreGive(preMux);
  }
  recModeOn = true;
  ev("REC mode: PSRAM %u -> %u KB free (stash and pre-roll released)",
     (unsigned)(before / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static void recModeExit() {
  if (!recModeOn) return;
  recModeOn = false;
  if (recModePrerollSlots > 0) prerollArm(recModePrerollSlots);
  recModePrerollSlots = 0;
  ev("REC mode off: PSRAM %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static bool shutterRecStart() {
  if (!shutterMux) return false;
  if (xSemaphoreTake(shutterMux, pdMS_TO_TICKS(200)) != pdTRUE) return false;
  bool ok = false;
  if (camOk && sdReady && !rec_is_active()) {
    String path = "/VID_" + String(recFileNum) + ".mov";
    recFileNum++;
    recModeEnter();
    rec_set_rotation(camViewRotNow());           // this clip carries the current view rotation
    ok = rec_start(path.c_str(), psramFound());
    ev("REC start %s -> %s", path.c_str(), ok ? "OK" : "FAILED");
    if (!ok) recModeExit();                      // nothing is recording: give everything back
  }
  xSemaphoreGive(shutterMux);
  return ok;
}

static String shutterRecStop() {
  if (!shutterMux) return String("");
  /* Finalising the QuickTime index on a long clip can take seconds. Timing
     out here would leave the recorder running with the caller told it had
     stopped -- worse than waiting. */
  if (xSemaphoreTake(shutterMux, pdMS_TO_TICKS(8000)) != pdTRUE) {
    ev("REC stop could not take the shutter lock");
    return String("");
  }
  String r = rec_stop();
  recModeExit();
  xSemaphoreGive(shutterMux);
  ev("REC stop -> %s", r.length() ? r.c_str() : "(was not recording)");
  return r;
}

static esp_err_t h_capture(httpd_req_t* req) {
  char raw[40] = {0}, who[28] = {0};
  if (qGet(req, "as", raw, sizeof(raw))) urlDecode(raw, who, sizeof(who));
  ShotResult r = shutterTake(who);
  return sendJson(req, "{\"result\":\"" + r.detail +
                       "\",\"count\":" + String(photoCount) + "}");
}

/* ── Diary: a note written beside the photo ───────────────────────────────
   Stored as a plain sidecar text file -- IMG_14.jpg gets IMG_14.txt -- so the
   notes survive being read on any computer, with no database and no index to
   corrupt. First line is the labels, the rest is whatever the user wrote. */
static esp_err_t h_note(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "{\"ok\":false,\"err\":\"no SD\"}");
  char fn[32];
  if (!qGet(req, "f", fn, sizeof(fn)))
    return sendJson(req, "{\"ok\":false,\"err\":\"no file\"}");
  for (size_t i = 0; fn[i]; i++) {                 // no traversal, no surprises
    char c = fn[i];
    bool ok = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-';
    if (!ok) return sendJson(req, "{\"ok\":false,\"err\":\"bad name\"}");
  }
  size_t len = req->content_len;
  if (len > 6144) return sendJson(req, "{\"ok\":false,\"err\":\"too long\"}");
  static char body[6145];
  size_t got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, body + got, len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) return ESP_FAIL;
    got += r;
  }
  body[got] = 0;

  /* The browser already knows the effect, its parameters, the subject count
     and the scene observation, so it composes BOTH representations and the
     camera only persists them. That keeps the ESP32 free of a JSON parser
     and of the string churn one would cause in a request path. The plain
     text file comes first and is authoritative -- it stays readable on any
     computer forever; the JSON is an addition, never a replacement. */
  const char* MARK = "\n===WUW-JSON===\n";
  char* split = strstr(body, MARK);
  size_t txtLen = split ? (size_t)(split - body) : got;
  const char* jsn = split ? split + strlen(MARK) : NULL;

  char dir[40] = "";
#ifdef EXHIBITION_MODE
  snprintf(dir, sizeof(dir), "%s", pasarSessionDir());
  SD_MMC.mkdir(dir);
#endif
  char tp[80], jp[80];
  snprintf(tp, sizeof(tp), "%s/%s.txt", dir, fn);
  snprintf(jp, sizeof(jp), "%s/%s.json", dir, fn);

  File f = SD_MMC.open(tp, FILE_WRITE);
  if (!f) { ev("NOTE open failed %s", tp);
            return sendJson(req, "{\"ok\":false,\"err\":\"open failed\"}"); }
  size_t w = f.write((const uint8_t*)body, txtLen);
  f.close();
  if (w != txtLen) { SD_MMC.remove(tp);        // half a note is worse than none
                     return sendJson(req, "{\"ok\":false,\"err\":\"SD full\"}"); }
  if (jsn && *jsn) {
    File g = SD_MMC.open(jp, FILE_WRITE);
    if (g) { g.write((const uint8_t*)jsn, strlen(jsn)); g.close(); }
  }
#ifdef EXHIBITION_MODE
  { int num = 0; if (sscanf(fn, "IMG_%d", &num) == 1) {
      pasarNoteWritten((uint32_t)num); wuwBridgeNote((uint32_t)num); } }
#endif
  ev("NOTE %s (%u bytes)", tp, (unsigned)txtLen);
  return sendJson(req, "{\"ok\":true,\"file\":\"" + String(tp) + "\"}");
}

static esp_err_t h_note_get(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "{\"ok\":false,\"err\":\"no SD\"}");
  char fn[32];
  if (!qGet(req, "f", fn, sizeof(fn)))
    return sendJson(req, "{\"ok\":false,\"err\":\"no file\"}");
  char tp[80];
#ifdef EXHIBITION_MODE
  snprintf(tp, sizeof(tp), "%s/%s.txt", pasarSessionDir(), fn);
#else
  snprintf(tp, sizeof(tp), "/%s.txt", fn);
#endif
  File f = SD_MMC.open(tp, FILE_READ);
  if (!f) return sendJson(req, "{\"ok\":true,\"text\":\"\"}");
  String t = f.readString();
  f.close();
  t.replace("\\", "\\\\"); t.replace("\"", "\\\""); t.replace("\n", "\\n");
  return sendJson(req, "{\"ok\":true,\"text\":\"" + t + "\"}");
}

/* The name this camera answers to. Persisted, and shown in the UI. */
static void devNameLoad() {
  Preferences p; p.begin("wuw", true);
  String n = p.getString("devname", "");
  p.end();
  if (n.length()) snprintf(devName, sizeof(devName), "%s", n.c_str());
}
static esp_err_t h_devname(httpd_req_t* req) {
  char q[48] = {0}, dec[48] = {0};
  if (qGet(req, "n", q, sizeof(q))) {
    urlDecode(q, dec, sizeof(dec));
    char clean[24]; size_t j = 0;
    for (size_t i = 0; dec[i] && j + 1 < sizeof(clean); i++) {
      char c = dec[i];
      bool ok = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
                c=='-'||c=='_'||c==' ';
      if (ok) clean[j++] = c;
    }
    clean[j] = 0;
    while (j && clean[j-1] == ' ') clean[--j] = 0;
    if (j) {
      snprintf(devName, sizeof(devName), "%s", clean);
      Preferences p; p.begin("wuw", false); p.putString("devname", devName); p.end();
      ev("device renamed to %s", devName);
    }
  }
  return sendJson(req, String("{\"ok\":true,\"name\":\"") + devName + "\"}");
}

/* ── Presets ─────────────────────────────────────────────────────────────
 * A look someone found and wants back. One small JSON per preset under
 * /PRESETS, so they survive a reflash and travel with the card. The browser
 * composes the JSON; the firmware only persists and returns it.
 */
#define PRESET_DIR "/PRESETS"
#define PRESET_MAX 4096

static bool presetName(httpd_req_t* req, char* out, size_t outN) {
  char raw[48] = {0}, dec[48] = {0};
  if (!qGet(req, "n", raw, sizeof(raw))) return false;
  urlDecode(raw, dec, sizeof(dec));
  size_t j = 0;
  for (size_t i = 0; dec[i] && j + 1 < outN && j < 24; i++) {
    char c = dec[i];
    bool ok = (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
              c=='-'||c=='_'||c==' ';
    if (ok) out[j++] = (c == ' ') ? '_' : c;    // spaces would break the path
  }
  out[j] = 0;
  return j > 0;
}

static esp_err_t h_preset_save(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "{\"ok\":false,\"err\":\"no SD\"}");
  char nm[28];
  if (!presetName(req, nm, sizeof(nm)))
    return sendJson(req, "{\"ok\":false,\"err\":\"name\"}");
  size_t len = req->content_len;
  if (!len || len > PRESET_MAX) return sendJson(req, "{\"ok\":false,\"err\":\"size\"}");
  static char body[PRESET_MAX + 1];
  size_t got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, body + got, len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) return ESP_FAIL;
    got += r;
  }
  body[got] = 0;
  SD_MMC.mkdir(PRESET_DIR);
  char path[64], tmp[64];
  snprintf(path, sizeof(path), PRESET_DIR "/%s.json", nm);
  snprintf(tmp,  sizeof(tmp),  PRESET_DIR "/%s.tmp",  nm);
  File f = SD_MMC.open(tmp, FILE_WRITE);
  if (!f) return sendJson(req, "{\"ok\":false,\"err\":\"open\"}");
  size_t w = f.write((const uint8_t*)body, got);
  f.close();
  /* temp-then-rename: losing power mid-write must not leave a preset that
     loads as garbage next time someone reaches for it */
  if (w != got) { SD_MMC.remove(tmp);
                  return sendJson(req, "{\"ok\":false,\"err\":\"SD full\"}"); }
  SD_MMC.remove(path);
  if (!SD_MMC.rename(tmp, path)) { SD_MMC.remove(tmp);
                  return sendJson(req, "{\"ok\":false,\"err\":\"rename\"}"); }
  ev("PRESET saved %s", nm);
  return sendJson(req, String("{\"ok\":true,\"name\":\"") + nm + "\"}");
}

static esp_err_t h_preset_load(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "{\"ok\":false,\"err\":\"no SD\"}");
  char nm[28];
  if (!presetName(req, nm, sizeof(nm)))
    return sendJson(req, "{\"ok\":false,\"err\":\"name\"}");
  char path[64];
  snprintf(path, sizeof(path), PRESET_DIR "/%s.json", nm);
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return sendJson(req, "{\"ok\":false,\"err\":\"not found\"}");
  httpd_resp_set_type(req, "application/json");
  static char chunk[1024];
  int r;
  while ((r = f.read((uint8_t*)chunk, sizeof(chunk))) > 0)
    if (httpd_resp_send_chunk(req, chunk, r) != ESP_OK) { f.close(); return ESP_FAIL; }
  f.close();
  return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t h_preset_list(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "[]");
  String out = "[";
  File dir = SD_MMC.open(PRESET_DIR);
  if (dir) {
    File f; int n = 0;
    while ((f = dir.openNextFile()) && n < 60) {
      const char* nm = f.name();
      const char* sl = strrchr(nm, '/');
      if (sl) nm = sl + 1;
      size_t L = strlen(nm);
      if (L > 5 && !strcasecmp(nm + L - 5, ".json")) {
        if (out.length() > 1) out += ",";
        out += "\"" + String(nm).substring(0, L - 5) + "\"";
        n++;
      }
      f.close();
    }
    dir.close();
  }
  out += "]";
  return sendJson(req, out);
}

static esp_err_t h_preset_del(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "{\"ok\":false}");
  char nm[28];
  if (!presetName(req, nm, sizeof(nm))) return sendJson(req, "{\"ok\":false}");
  char path[64];
  snprintf(path, sizeof(path), PRESET_DIR "/%s.json", nm);
  bool ok = SD_MMC.remove(path);
  ev("PRESET delete %s -> %s", nm, ok ? "ok" : "missing");
  return sendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"not found\"}");
}


#ifdef EXHIBITION_MODE
/* The exhibition poll. Every visitor hits this every couple of seconds, so it
   allocates nothing and enumerates nothing. It also doubles as the presence
   heartbeat: asking for room state IS how a visitor says they are still here. */
static esp_err_t h_room(httpd_req_t* req) {
  char raw[40] = {0}, a[40] = {0}, alias[24] = {0};
  if (qGet(req, "as", raw, sizeof(raw))) urlDecode(raw, a, sizeof(a));
  int before = pasarCount();
  pasarTouch(a, alias, sizeof(alias));
  int after = pasarCount();
  if (after > before) wuwBridgeJoin(alias);
  wuwBridgeClientCount(after);
  /* Assembled into a fixed buffer. Twenty visitors polling every 2.5 s for
     six hours is ~170,000 of these; String concatenation there is precisely
     how the heap fragments until an allocation fails at hour five. */
  static char out[420];
  String j = pasarRoomJson(camOk, sdReady, devName);
  int n = snprintf(out, sizeof(out), "%s", j.c_str());
  if (n > 1 && n < (int)sizeof(out) - 40) {
    n--;                                          // drop the closing brace
    snprintf(out + n, sizeof(out) - n, ",\"you\":\"%s\"}", alias);
  }
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out, HTTPD_RESP_USE_STRLEN);
}

/* Admin only, and deliberately not linked from the public interface. */
static esp_err_t h_session_new(httpd_req_t* req) {
  char k[24] = {0};
  if (!qGet(req, "k", k, sizeof(k)) || strcmp(k, "wuw01")) {
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_send(req, "admin only", HTTPD_RESP_USE_STRLEN);
  }
  pasarNewSession();
  ev("PASAR new session %s", pasarSession());
  return sendJson(req, "{\"ok\":true,\"session\":\"" + String(pasarSession()) + "\"}");
}

/* ── Live archive ────────────────────────────────────────────────────────
 * A projection wall may poll this every couple of seconds for six hours, so
 * the directory is enumerated only when the photo count actually changes. */
static char     archBuf[6144];
static size_t   archLen  = 0;
static int      archStamp = -1;          // photoCount the cache was built at

static void archRebuild() {
  archLen = 0;
  archBuf[archLen++] = '[';
  File dir = SD_MMC.open(pasarSessionDir());
  if (dir) {
    File f;
    int guard = 0;
    while ((f = dir.openNextFile()) && guard++ < 400) {
      const char* nm = f.name();
      const char* sl = strrchr(nm, '/');
      if (sl) nm = sl + 1;
      int n;
      /* sscanf returns the count of ASSIGNED conversions, not whether the
         literal tail matched -- so "IMG_0016.txt" and ".json" both satisfied
         "IMG_%d.jpg" and every photograph appeared three times in the index,
         once per sidecar. Check the extension explicitly. */
      size_t nl = strlen(nm);
      bool isJpg = (nl > 4) && !strcasecmp(nm + nl - 4, ".jpg");
      if (isJpg && sscanf(nm, "IMG_%d", &n) == 1 && !strstr(nm, "_view")) {
        char probe[80];
        snprintf(probe, sizeof(probe), "%s/IMG_%04d.txt", pasarSessionDir(), n);
        bool hasNote = SD_MMC.exists(probe);
        snprintf(probe, sizeof(probe), "%s/IMG_%04d_view.jpg", pasarSessionDir(), n);
        bool hasView = SD_MMC.exists(probe);
        char item[48];
        int w = snprintf(item, sizeof(item), "%s{\"n\":%d,\"t\":%d,\"v\":%d}",
                         archLen > 1 ? "," : "", n, hasNote ? 1 : 0, hasView ? 1 : 0);
        if (archLen + w + 2 < sizeof(archBuf)) {
          memcpy(archBuf + archLen, item, w); archLen += w;
        }
      }
      f.close();
    }
    dir.close();
  }
  archBuf[archLen++] = ']';
  archBuf[archLen]   = 0;
  archStamp = photoCount;
}

static esp_err_t h_arch_list(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "[]");
  if (archStamp != photoCount) archRebuild();
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  /* So a phone on WUW-02's page can list WUW-01's roll without this device
     proxying a single byte. The frames are already public to anyone on the
     network; this only removes the browser's same-origin objection. */
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, archBuf, archLen);
}

/* Serves one frame out of the session folder. Prefers the visitor's rendered
   derivative when one exists, because the originals are 5 MP. */
static esp_err_t h_arch_img(httpd_req_t* req) {
  if (!sdReady) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no card"); return ESP_FAIL; }
  int n = -1;
  char q[16];
  if (qGet(req, "n", q, sizeof(q))) n = atoi(q);
  if (n < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "n"); return ESP_FAIL; }
  bool wantFull = false;
  if (qGet(req, "full", q, sizeof(q))) wantFull = (q[0] == '1');

  char path[80];
  if (!wantFull) {
    snprintf(path, sizeof(path), "%s/IMG_%04d_view.jpg", pasarSessionDir(), n);
    if (!SD_MMC.exists(path))
      snprintf(path, sizeof(path), "%s/IMG_%04d.jpg", pasarSessionDir(), n);
  } else {
    snprintf(path, sizeof(path), "%s/IMG_%04d.jpg", pasarSessionDir(), n);
  }
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no image"); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");  // frames never change
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");   // peer browsing
  static uint8_t chunk[4096];
  int r;
  while ((r = f.read(chunk, sizeof(chunk))) > 0)
    if (httpd_resp_send_chunk(req, (const char*)chunk, r) != ESP_OK) { f.close(); return ESP_FAIL; }
  f.close();
  return httpd_resp_send_chunk(req, NULL, 0);
}

/* ── The VIEW derivative ─────────────────────────────────────────────────
 * The ORIGINAL off the sensor is already safely on the card before this can
 * be called. This is the visitor's own rendering, strictly optional, and
 * streamed straight to SD rather than buffered in PSRAM. */
#define VIEW_MAX (512 * 1024)

static esp_err_t h_view(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "{\"ok\":false,\"err\":\"no SD\"}");
  int n = -1; char q[16];
  if (qGet(req, "n", q, sizeof(q))) n = atoi(q);
  if (n < 0) return sendJson(req, "{\"ok\":false,\"err\":\"n\"}");

  size_t len = req->content_len;
  if (!len || len > VIEW_MAX) {
    ev("VIEW rejected %u bytes", (unsigned)len);
    return sendJson(req, "{\"ok\":false,\"err\":\"size\"}");
  }
  char tmp[80], path[80];
  snprintf(path, sizeof(path), "%s/IMG_%04d_view.jpg", pasarSessionDir(), n);
  snprintf(tmp,  sizeof(tmp),  "%s/IMG_%04d_view.tmp", pasarSessionDir(), n);

  File f = SD_MMC.open(tmp, FILE_WRITE);
  if (!f) return sendJson(req, "{\"ok\":false,\"err\":\"open\"}");
  static uint8_t chunk[2048];
  size_t got = 0;
  bool bad = false;
  while (got < len) {
    int want = (int)((len - got) < sizeof(chunk) ? (len - got) : sizeof(chunk));
    int r = httpd_req_recv(req, (char*)chunk, want);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) { bad = true; break; }
    if (f.write(chunk, r) != (size_t)r) { bad = true; break; }   // card full
    got += r;
  }
  f.close();
  /* Written under a temporary name and renamed only once complete, so a
     dropped connection can never leave a half-image the archive would show. */
  if (bad || got != len) {
    SD_MMC.remove(tmp);
    ev("VIEW incomplete %u/%u", (unsigned)got, (unsigned)len);
    return sendJson(req, "{\"ok\":false,\"err\":\"incomplete\"}");
  }
  SD_MMC.remove(path);
  if (!SD_MMC.rename(tmp, path)) { SD_MMC.remove(tmp);
    return sendJson(req, "{\"ok\":false,\"err\":\"rename\"}"); }
  archStamp = -1;                       // the archive index is now stale
  ev("VIEW %s (%u KB)", path, (unsigned)(got / 1024));
  return sendJson(req, "{\"ok\":true}");
}

/* The archive page. Served from flash, no external asset of any kind. */
static const char ARCHIVE_HTML[] PROGMEM = R"arch(<!doctype html><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>WUW-01 ARCHIVE</title><style>
*{box-sizing:border-box}html,body{margin:0;background:#07070b;color:#c9c6d6;
font:14px/1.5 system-ui,-apple-system,sans-serif;-webkit-font-smoothing:antialiased}
header{display:flex;justify-content:space-between;align-items:baseline;gap:12px;
padding:12px 16px;border-bottom:1px solid #21212e;position:sticky;top:0;
background:#07070b;z-index:5}
h1{margin:0;font-size:13px;letter-spacing:.26em;font-weight:600;color:#efedf6}
header em{font-style:normal;font-size:11px;letter-spacing:.16em;color:#6f6c85}
#g{display:grid;grid-template-columns:repeat(auto-fill,minmax(132px,1fr));
gap:7px;padding:12px}
figure{margin:0;position:relative;aspect-ratio:4/3;overflow:hidden;border-radius:7px;
background:#12121b;border:1px solid #21212e;cursor:pointer}
figure img{width:100%;height:100%;object-fit:cover;display:block}
figcaption{position:absolute;left:0;right:0;bottom:0;display:flex;
justify-content:space-between;gap:6px;padding:4px 7px;font-size:10px;
letter-spacing:.1em;background:linear-gradient(0deg,rgba(4,4,8,.85),transparent);
color:#d6d3e2}
.note{color:#a99ae8}
#full{position:fixed;inset:0;background:rgba(4,4,8,.97);display:none;
flex-direction:column;align-items:center;justify-content:center;gap:12px;
padding:16px;z-index:9}
#full.on{display:flex}
#full img{max-width:100%;max-height:70vh;object-fit:contain;border-radius:8px}
#full pre{max-width:640px;width:100%;max-height:22vh;overflow:auto;margin:0;
white-space:pre-wrap;font:12px/1.6 ui-monospace,monospace;color:#b6b3c6;
background:#101018;border:1px solid #21212e;border-radius:8px;padding:11px}
button{font:inherit;padding:8px 15px;border-radius:8px;border:1px solid #33334a;
background:#16161f;color:#e6e3f0;cursor:pointer}
body.proj header{display:none}
body.proj #g{grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:4px;padding:4px}
</style>
<header><h1>WUW-01 ARCHIVE</h1><em id=meta>--</em>
<button onclick="document.body.classList.toggle('proj')">PROJECT</button></header>
<div id=g></div>
<div id=full onclick="this.classList.remove('on')">
  <img id=fimg alt=""><pre id=ftxt></pre><button>CLOSE</button></div>
<script>
var seen={},G=document.getElementById('g');
function pad(n){return String(n).padStart(4,'0')}
function add(it){
  if(seen[it.n])return; seen[it.n]=1;
  var fg=document.createElement('figure');
  var im=document.createElement('img');
  im.loading='lazy'; im.decoding='async';   /* never load the whole wall at once */
  im.src='/archive/img?n='+it.n;
  var cap=document.createElement('figcaption');
  cap.innerHTML='<span>'+pad(it.n)+'</span>'+(it.t?'<span class=note>NOTE</span>':'');
  fg.appendChild(im); fg.appendChild(cap);
  fg.onclick=function(){ open_(it.n) };
  G.insertBefore(fg,G.firstChild);          /* newest first */
}
function open_(n){
  document.getElementById('fimg').src='/archive/img?n='+n+'&full=1';
  var t=document.getElementById('ftxt'); t.textContent='';
  fetch('/note?f=IMG_'+pad(n)).then(function(r){return r.json()})
    .then(function(d){ t.textContent=(d&&d.text)||'(no note)' }).catch(function(){});
  document.getElementById('full').classList.add('on');
}
function tick(){
  fetch('/archive/list',{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(a){ a.sort(function(x,y){return x.n-y.n}); a.forEach(add); })
    .catch(function(){});
  fetch('/room',{cache:'no-store'}).then(function(r){return r.json()})
    .then(function(d){ if(!d||!d.exhibition)return;
      document.getElementById('meta').textContent=
        d.session+'  ·  '+d.visitors+(d.visitors===1?' PERSON':' PEOPLE')+
        '  ·  '+d.photos+' IMAGES'; }).catch(function(){});
}
tick(); setInterval(tick,3000);
</script>)arch";

static esp_err_t h_archive(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, ARCHIVE_HTML, HTTPD_RESP_USE_STRLEN);
}
#endif


#ifdef WUW_PANEL
static esp_err_t h_panel_bwo_wall(httpd_req_t* req) {
  if (!sdReady)
    return sendJson(req, "{\"ok\":false,\"err\":\"no SD card\"}");
  char raw[128] = {0};
  if (qGet(req, "f", raw, sizeof(raw))) {
    char decoded[80] = {0};
    urlDecode(raw, decoded, sizeof(decoded));
    const char* name = decoded;
    while (*name == '/') ++name;
    const char* kind = nullptr;
    if (strlen(name) >= 64 || !sdSafeMediaPath(name, &kind) ||
        strcmp(kind, "jpg"))
      return sendJson(req, "{\"ok\":false,\"err\":\"invalid JPEG path\"}");
    char path[80];
    snprintf(path, sizeof(path), "/%s", name);
    File file = SD_MMC.open(path, FILE_READ);
    bool valid = file && !file.isDirectory();
    if (file) file.close();
    if (!valid)
      return sendJson(req, "{\"ok\":false,\"err\":\"photo missing\"}");
    bool ok = panelSetBwoWall(path);
    return sendJson(req, ok ? "{\"ok\":true}" :
                              "{\"ok\":false,\"err\":\"invalid photo\"}");
  }
  int n = -1;
  if (!qInt(req, "n", &n) || n < 0)
    return sendJson(req, "{\"ok\":false,\"err\":\"select an SD photo\"}");
  char path[80];
#ifdef EXHIBITION_MODE
  snprintf(path, sizeof(path), "%s/IMG_%04d.jpg", pasarSessionDir(), n);
#else
  snprintf(path, sizeof(path), "/IMG_%d.jpg", n);
#endif
  if (!SD_MMC.exists(path))
    return sendJson(req, "{\"ok\":false,\"err\":\"photo missing\"}");
  bool ok = panelSetBwoWall(path);
  return sendJson(req, ok ? "{\"ok\":true}" :
                            "{\"ok\":false,\"err\":\"invalid photo\"}");
}

static esp_err_t h_panel_skin(httpd_req_t* req) {
  char raw[32] = {0}, name[24] = {0};
  if (qGet(req, "name", raw, sizeof(raw))) urlDecode(raw, name, sizeof(name));
  bool ok = panelSetSkin(name);
  return sendJson(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"unknown skin\"}");
}

/* Admin: turn the screen on once it is physically attached. Persisted, and
   applied at the next boot -- bringing SPI up underneath a running camera is
   not worth the risk for a setting changed once in the device's life. */
static esp_err_t h_panel(httpd_req_t* req) {
  char k[24] = {0};
  if (!qGet(req, "k", k, sizeof(k)) || strcmp(k, "wuw01")) {
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_send(req, "admin only", HTTPD_RESP_USE_STRLEN);
  }
  /* auto = probe the panel and believe it; on = run regardless; off = never.
     "on=1"/"on=0" still work so anything already scripted keeps working. */
  char v[12] = {0};
  int want = -1;
  if (qGet(req, "mode", v, sizeof(v))) {
    if      (!strcmp(v, "auto")) want = 0;
    else if (!strcmp(v, "on"))   want = 1;
    else if (!strcmp(v, "off"))  want = 2;
    else return sendJson(req, "{\"ok\":false,\"err\":\"mode is auto, on or off\"}");
  } else if (qGet(req, "on", v, sizeof(v))) {
    want = (v[0] == '1') ? 1 : 2;
  }
  /* Which way up. 1 and 3 are the two unmirrored landscape rotations, 180
     apart; 5 and 7 are the same pair mirrored and are refused rather than
     stored, because a mirrored UI is never what anyone meant. Which of 1 or 3
     is right depends only on which end the ribbon leaves the case, so it is
     the operator's call and not a compile-time constant. */
  { int r;
    if (qInt(req, "rot", &r)) {
      if (r != 1 && r != 3)
        return sendJson(req, "{\"ok\":false,\"err\":\"rot is 1 or 3; "
                             "5 and 7 are the same views mirrored\"}");
      Preferences pp; pp.begin("wuw", false);
      pp.putUChar("panelrot", (uint8_t)r);
      pp.end();
      ev("PANEL rotation %d (next boot)", r);
    } }
  /* SPI clock, in MHz. This jumper harness is verified clean only to 10 MHz.
     Higher stale values used to persist in NVS and corrupt production UI. */
  { int h;
    if (qInt(req, "hz", &h)) {
      if (h < 4 || h > 10)
        return sendJson(req, "{\"ok\":false,\"err\":\"hz is 4..10 MHz on this harness\"}");
      Preferences pp; pp.begin("wuw", false);
      pp.putUChar("panelhz", (uint8_t)h);
      pp.end();
      ev("PANEL SPI %d MHz (next boot)", h);
    } }
  /* Re-run touch calibration on the next boot. Three non-collinear points
     fit the affine map. A fresh device asks automatically. */
  { int cb;
    if (qInt(req, "calib", &cb) && cb) {
      Preferences pp; pp.begin("wuw", false);
      pp.putBool("tcalreq", true);
      pp.end();
      ev("PANEL touch calibration requested (next boot)");
    } }
  if (want >= 0) {
    Preferences pp; pp.begin("wuw", false);
    pp.putUChar("panelm", (uint8_t)want);
    pp.end();
    ev("PANEL mode %s (next boot)",
       want == 0 ? "auto" : want == 1 ? "forced on" : "forced off");
  }
  uint8_t mode;
  { Preferences pp; pp.begin("wuw", true);
    mode = pp.getUChar("panelm", 0xFF);
    if (mode == 0xFF) mode = pp.getBool("panel", false) ? 1 : 0;
    pp.end(); }
  /* Changed-tile push, so it can be turned off and compared on real glass
     rather than believed. Applies immediately; no reboot. */
  { int t;
    if (qInt(req, "tile", &t))   panelTileConfig(t ? 1 : 0, -1);
    if (qInt(req, "thresh", &t)) panelTileConfig(-1, t); }
  char ts[192] = "{}";
  panelTileStats(ts, sizeof(ts));

  uint8_t rotNow, hzNow;
  { Preferences pp; pp.begin("wuw", true);
    rotNow = pp.getUChar("panelrot", 1);
    hzNow  = pp.getUChar("panelhz", 10); pp.end(); }
  const char* mn = mode == 2 ? "off" : "on";
  return sendJson(req, String("{\"ok\":true,\"push\":") + ts + ",\"mode\":\"" + mn +
                       "\",\"rot\":" + String(rotNow) + ",\"hz\":" + String(hzNow) +
                       ",\"running\":" + (panelPresent() ? "true" : "false") +
                       ",\"touch\":" + (panelTouchReady() ? "true" : "false") +
                       ",\"note\":\"applies on next boot\"}");
}
#endif

/* ── WUW LINK ─────────────────────────────────────────────────────────────
 * Four endpoints. Three of them are read-only or local-only; the one that
 * causes anything to happen elsewhere is /link/fire, and all it can ask a
 * peer to do is photograph its own subject.
 *
 * Pulling is deliberately one-sided: this device fetches FROM a peer. There
 * is no endpoint by which a peer writes TO us, so joining a mesh cannot put
 * a file on our card. */
#ifdef WUW_LINK
static esp_err_t h_link_peers(httpd_req_t* req) {
  /* Polled every couple of seconds by every open browser, so it is written
     into a fixed buffer rather than built with String concatenation. */
  static char buf[2048];
  size_t n = linkPeersJson(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, buf, n);
}

static esp_err_t h_link_fire(httpd_req_t* req) {
  int lead = 400;                      // long enough for a slow peer to hear it
  qInt(req, "lead", &lead);
  char raw[40] = {0}, tag[16] = {0};
  if (qGet(req, "tag", raw, sizeof(raw))) urlDecode(raw, tag, sizeof(tag));
  bool ok = linkFire((uint16_t)lead, tag[0] ? tag : devName);
  ev("LINK fire in %dms -> %d peers", lead, linkPeerCount());
  return sendJson(req, String("{\"ok\":") + (ok ? "true" : "false") +
                       ",\"lead\":" + String(lead) +
                       ",\"peers\":" + String(linkPeerCount()) + "}");
}

static esp_err_t h_link_pull(httpd_req_t* req) {
  if (!sdReady) return sendJson(req, "{\"ok\":false,\"err\":\"no SD\"}");
  char ip[20] = {0}, nm[24] = {0};
  int n = -1;
  if (!qGet(req, "ip", ip, sizeof(ip)) || !qInt(req, "n", &n) || n < 0)
    return sendJson(req, "{\"ok\":false,\"err\":\"need ip and n\"}");
  /* Dotted quad only. The string ends up in a connect() call and nowhere
     near a shell or a path, but it arrives from a browser, so it is checked
     rather than trusted. */
  IPAddress probe;
  if (!probe.fromString(ip))
    return sendJson(req, "{\"ok\":false,\"err\":\"bad address\"}");
  char raw[40] = {0};
  if (qGet(req, "as", raw, sizeof(raw))) urlDecode(raw, nm, sizeof(nm));
  bool ok = linkPullStart(ip, n, nm);
  return sendJson(req, String("{\"ok\":") + (ok ? "true" : "false") +
                       ",\"busy\":" + (linkPullBusy() ? "true" : "false") +
                       ",\"status\":\"" + linkPullStatus() + "\"}");
}

static esp_err_t h_link_config(httpd_req_t* req) {
  char k[24] = {0};
  if (!qGet(req, "k", k, sizeof(k)) || strcmp(k, "wuw01")) {
    httpd_resp_set_status(req, "403 Forbidden");
    return httpd_resp_send(req, "admin only", HTTPD_RESP_USE_STRLEN);
  }
  char raw[40] = {0}, g[16] = {0};
  if (qGet(req, "group", raw, sizeof(raw))) {
    urlDecode(raw, g, sizeof(g));
    if (g[0]) { linkSetGroup(g); ev("LINK group -> %s", linkGroup()); }
  }
  return sendJson(req, String("{\"ok\":true,\"group\":\"") + linkGroup() +
                       "\",\"up\":" + (linkUp() ? "true" : "false") + "}");
}
#endif

/* Writes the ring out oldest-first. Runs on the request task and can take a
   second or two for a full ring; that is a deliberate trade -- the frames are
   in volatile memory and every millisecond spent negotiating is a millisecond
   they could be overwritten. Feeding is locked out for the duration. */
static int prerollSave(char* firstOut, size_t firstCap) {
  if (!sdReady || !preMux) return 0;
  if (xSemaphoreTake(preMux, pdMS_TO_TICKS(1000)) != pdTRUE) return 0;
  int written = 0;
  if (preSlots > 0 && preStored > 0) {
    static uint32_t burst = 0;
    burst++;
    /* Oldest first: start at the head (the next slot to be overwritten, i.e.
       the eldest) and walk forward. A ring that has not filled yet has empty
       slots, which carry len 0 and are skipped. */
    for (int k = 0; k < preSlots; k++) {
      PreSlot& sl = preRing[(preHead + k) % preSlots];
      if (!sl.len) continue;
      char path[80];
#ifdef EXHIBITION_MODE
      snprintf(path, sizeof(path), "%s/PRE_%03u_%02d.jpg",
               pasarSessionDir(), (unsigned)(burst % 1000), written);
#else
      snprintf(path, sizeof(path), "/PRE_%03u_%02d.jpg",
               (unsigned)(burst % 1000), written);
#endif
      File f = SD_MMC.open(path, FILE_WRITE);
      if (!f) break;
      size_t w = jpegWriteOriented(fileSink, &f, sl.buf, sl.len, camViewRotNow());
      f.close();
      if (!w) { SD_MMC.remove(path); break; }            // card full
      if (written == 0 && firstOut) snprintf(firstOut, firstCap, "%s", path);
      written++;
    }
  }
  xSemaphoreGive(preMux);
  return written;
}

static esp_err_t h_preroll(httpd_req_t* req) {
  int v;
  if (qInt(req, "on", &v)) {
    if (v > 0 && rec_is_active())
      return sendJson(req, "{\"ok\":false,\"err\":\"recording - arm pre-roll after it stops\"}");
    if (v > 0) {
      int want = 24;                       // ~3 s at the rate a viewer polls
      qInt(req, "slots", &want);
      prerollArm(want);
    } else if (preMux && xSemaphoreTake(preMux, pdMS_TO_TICKS(500)) == pdTRUE) {
      prerollFree();
      xSemaphoreGive(preMux);
      ev("PREROLL released");
    }
  }
  uint32_t span = 0;                       // seconds the ring actually covers
  if (preSlots > 0 && preStored > 0) {
    int n = (int)(preStored < (uint32_t)preSlots ? preStored : (uint32_t)preSlots);
    uint32_t oldest = preRing[(preHead + preSlots - n) % preSlots].ms;
    uint32_t newest = preRing[(preHead + preSlots - 1) % preSlots].ms;
    if (newest > oldest) span = newest - oldest;
  }
  return sendJson(req, String("{\"ok\":true,\"on\":") + (preOn ? "true" : "false") +
                       ",\"slots\":" + String(preSlots) +
                       ",\"held\":" + String(preStored < (uint32_t)preSlots
                                              ? preStored : (uint32_t)preSlots) +
                       ",\"spanMs\":" + String(span) +
                       ",\"kb\":" + String(preSlots * (PREROLL_SLOT_BYTES / 1024)) +
                       ",\"psramFree\":" +
                         String((uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) + "}");
}

static esp_err_t h_preroll_save(httpd_req_t* req) {
  char first[80] = "";
  int n = prerollSave(first, sizeof(first));
  if (n > 0) ev("PREROLL saved %d frames -> %s", n, first);
  return sendJson(req, String("{\"ok\":") + (n > 0 ? "true" : "false") +
                       ",\"frames\":" + String(n) +
                       ",\"first\":\"" + String(first) + "\"}");
}

/* ── Task stacks, measured ────────────────────────────────────────────────
 * Every stack size in this firmware is a guess: 3072 here, 6144 there, 12288
 * for the one that does TLS. A guess that is too small does not fail at boot
 * or in a test -- it fails the first time a particular call path runs deep,
 * which on an exhibition camera means hour five, in front of people, as a
 * reboot with no explanation.
 *
 * uxTaskGetStackHighWaterMark reports the LOW WATER MARK: the least free
 * space that task has ever had since it started. It is the only number that
 * matters, because it already accounts for the worst path taken so far.
 *
 * Leave the camera running for a few hours, then read this. Anything under
 * about 512 bytes of headroom is living on borrowed time; anything with
 * kilobytes spare is RAM that could be doing something else. */
static esp_err_t h_tasks(httpd_req_t* req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  static char buf[1536];
  size_t w = 0;
  w += snprintf(buf + w, sizeof(buf) - w,
                "{\"ok\":true,\"heap\":%u,\"heapMin\":%u,\"psram\":%u,\"tasks\":[",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)ESP.getFreePsram());

  /* Our own tasks by name, plus the two the framework makes. Looked up by
     name rather than kept as handles: a task that failed to start then shows
     up as absent instead of as a stale pointer. */
  static const char* names[] = { "loopTask", "httpd", "stream_httpd", "shot",
                                 "btn", "panel", "link", "lpull", "rec" };
  bool first = true;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    TaskHandle_t h = xTaskGetHandle(names[i]);
    if (!h) continue;
    /* ESP-IDF returns BYTES here, unlike vanilla FreeRTOS which returns words.
       Getting that wrong makes every stack look four times safer than it is. */
    UBaseType_t free_ = uxTaskGetStackHighWaterMark(h);
    BaseType_t core;
#if ESP_IDF_VERSION_MAJOR >= 5
    core = xTaskGetCoreID(h);
#else
    core = xTaskGetAffinity(h);
#endif
    if (w + 96 >= sizeof(buf)) break;
    w += snprintf(buf + w, sizeof(buf) - w,
                  "%s{\"n\":\"%s\",\"freeB\":%u,\"core\":%d,\"prio\":%u}",
                  first ? "" : ",", names[i], (unsigned)free_,
                  (int)core, (unsigned)uxTaskPriorityGet(h));
    first = false;
  }
  w += snprintf(buf + w, sizeof(buf) - w, "],\"upSec\":%u}",
                (unsigned)(millis() / 1000));
  return httpd_resp_send(req, buf, w);
}

static esp_err_t h_night(httpd_req_t* req) {
  int v;
  if (qInt(req, "v", &v)) {
    nightVision = v != 0;
    if (camOk) applyNightVision(nightVision);
  }
  return sendJson(req, "{\"night\":" + String(nightVision ? "true" : "false") + "}");
}

static esp_err_t h_set(httpd_req_t* req) {
  if (!camOk) return sendJson(req, "{\"ok\":false,\"err\":\"camera offline\"}");
  sensor_t* s = esp_camera_sensor_get();
  int v;
  if (qInt(req, "effect",      &v)) s->set_special_effect(s, v);
  if (qInt(req, "bright",      &v)) {
    camBrightness = constrain(v, -2, 2);
    s->set_brightness(s, camBrightness);
    prefs.putInt("bright", camBrightness);
  }
  if (qInt(req, "contrast",    &v)) s->set_contrast(s, v);
  if (qInt(req, "sat",         &v)) { v = constrain(v, -2, 2); s->set_saturation(s, v); prefs.putInt("sat", v); }
  if (qInt(req, "sharpness",   &v)) s->set_sharpness(s, v);
  if (qInt(req, "quality",     &v)) {
    streamQ = constrain(v, 8, 45); s->set_quality(s, streamQ);
    prefs.putInt("streamq", streamQ);
  }
  if (qInt(req, "photoq",      &v)) {
    photoQ = constrain(v, 4, 20); prefs.putInt("photoq", photoQ);
  }
  if (qInt(req, "denoise",     &v)) { if (s->set_denoise) s->set_denoise(s, v); }
  if (qInt(req, "wb",          &v)) { v = constrain(v, 0, 4); s->set_wb_mode(s, v); prefs.putInt("wbmode", v); }
  if (qInt(req, "viewrot",     &v)) camSetViewRot((uint8_t)(v & 3));
  if (qInt(req, "hmirror",     &v)) {
    camHMirror = v != 0; s->set_hmirror(s, camHMirror);
    prefs.putBool("hmirror", camHMirror);
  }
  if (qInt(req, "vflip",       &v)) {
    camVFlip = v != 0; s->set_vflip(s, camVFlip);
    prefs.putBool("vflip", camVFlip);
  }
  if (qInt(req, "aec",         &v)) s->set_exposure_ctrl(s, v);
  if (qInt(req, "agc",         &v)) s->set_gain_ctrl(s, v);
  if (qInt(req, "ae_level",    &v)) {
    camAeLevel = constrain(v, -2, 2); s->set_ae_level(s, camAeLevel);
    prefs.putInt("aelevel", camAeLevel);
  }
  if (qInt(req, "aec_val",     &v)) s->set_aec_value(s, v);
  if (qInt(req, "agc_gain",    &v)) s->set_agc_gain(s, v);
  if (qInt(req, "gainceiling", &v)) s->set_gainceiling(s, (gainceiling_t)v);
  if (qInt(req, "lenc",        &v)) s->set_lenc(s, v);
  if (qInt(req, "raw_gma",     &v)) s->set_raw_gma(s, v);
  if (qInt(req, "dcw",         &v)) s->set_dcw(s, v);
  if (qInt(req, "res", &v) && !rec_is_active() && validFrameSize(v)) {
    streamRes = (uint8_t)v;
    s->set_framesize(s, (framesize_t)streamRes);
    prefs.putUChar("streamres", streamRes);
    lightModeChanged();
    Serial.printf("[SET] stream res -> %d\n", streamRes);
  }
  if (qInt(req, "photores", &v) && validFrameSize(v)) {
    photoRes = (uint8_t)v;
    prefs.putUChar("photores", photoRes);
    Serial.printf("[SET] photo res -> %d\n", photoRes);
  }
  return sendJson(req, "{\"ok\":true}");
}

static esp_err_t h_rec_start(httpd_req_t* req) {
  if (!camOk)           { ev("REC refused: camera offline");
                          return sendJson(req, "{\"ok\":false,\"err\":\"Camera offline\"}"); }
  if (!sdReady)         { ev("REC refused: no SD");
                          return sendJson(req, "{\"ok\":false,\"err\":\"No SD card\"}"); }
  if (rec_is_active())  { ev("REC refused: already recording");
                          return sendJson(req, "{\"ok\":false,\"err\":\"Already recording\"}"); }
  bool ok = shutterRecStart();
  return sendJson(req, ok ? "{\"ok\":true,\"file\":\"/VID_" + String(recFileNum - 1) + ".mov\"}"
                          : "{\"ok\":false,\"err\":\"Start failed\"}");
}

static esp_err_t h_rec_stop(httpd_req_t* req) {
  String r = shutterRecStop();
  return sendJson(req, "{\"ok\":true,\"result\":\"" + r + "\"}");
}

/* What the recorder is actually doing: real frame rate, frames dropped, how deep
   the ring got, the slowest SD write. "Laggy" is a feeling; this is the number. */
static esp_err_t h_rec_stats(httpd_req_t* req) {
  static char out[420];
  size_t n = rec_stats_json(out, sizeof(out));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, out, n);
}

static esp_err_t h_rec_config(httpd_req_t* req) {
  int fps = 30, fx = 0, inten = 5, dly = 3;      // fps is a ceiling; the sensor sets the rate
  qInt(req, "fps", &fps);
  qInt(req, "effect", &fx);
  qInt(req, "intensity", &inten);
  qInt(req, "delay", &dly);
  rec_set_config((uint8_t)fps, (uint8_t)fx, (uint8_t)inten, (uint8_t)dly);
  return sendJson(req, "{\"ok\":true}");
}

// ── MJPEG stream (port 81) — desktop convenience ─────────────────────────
#define BOUNDARY "wuwcam_frame"
static esp_err_t h_stream(httpd_req_t* req) {
  CamFrame fb;
  uint32_t lastSeq = 0;
  char hdr[80];

  if (!camOk) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera offline");
    return ESP_FAIL;
  }
  if (httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY) != ESP_OK)
    return ESP_FAIL;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  while (true) {
    if (!camFrameAcquire(&fb)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    /* While recording this is the recorder's shared frame, and the same one stays
       current until the next arrives. Without this check the loop would re-send
       it every 5 ms -- a flood of duplicates. */
    if (fb.slot >= 0 && fb.seq == lastSeq) {
      camFrameRelease(&fb); vTaskDelay(pdMS_TO_TICKS(8)); continue;
    }
    lastSeq = fb.seq;

    int n = snprintf(hdr, sizeof(hdr),
                     "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                     "Content-Length: %u\r\n\r\n", (unsigned)fb.len);

    esp_err_t r = httpd_resp_send_chunk(req, hdr, n);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char*)fb.buf, fb.len);
    prerollFeed(fb.buf, fb.len);          // the MJPEG viewer fills it too
    camFrameRelease(&fb);

    if (r != ESP_OK) break;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════════════
//  SERVER START
// ═══════════════════════════════════════════════════════════════════════
/* Nagle waits for more data before sending a partial segment — great for bulk
   transfer, poison for a per-frame protocol. Disable it as each socket opens. */
static esp_err_t sockOpen(httpd_handle_t hd, int sockfd) {
  int one = 1;
  setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return ESP_OK;
}

static int nReg = 0, wildIdx = -1;
static void reg(httpd_handle_t h, const char* uri, esp_err_t (*fn)(httpd_req_t*)) {
  httpd_uri_t u = { .uri = uri, .method = HTTP_GET, .handler = fn, .user_ctx = NULL };
  if (httpd_register_uri_handler(h, &u) == ESP_OK) {
    if (strchr(uri, '*')) wildIdx = nReg;
    nReg++;
  } else Serial.printf("[WEB] register FAILED: %s\n", uri);
}
static void regPost(httpd_handle_t h, const char* uri, esp_err_t (*fn)(httpd_req_t*)) {
  httpd_uri_t u = { .uri = uri, .method = HTTP_POST, .handler = fn, .user_ctx = NULL };
  httpd_register_uri_handler(h, &u);
}

static void startServers() {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port      = 80;
  c.ctrl_port        = 32768;
  c.core_id          = 1;
  c.task_priority    = 5;
  c.stack_size       = 12288;
  c.max_uri_handlers = 64;
  c.max_open_sockets = 7;
  c.lru_purge_enable = true;
  c.uri_match_fn     = httpd_uri_match_wildcard;
  c.recv_wait_timeout = 12;   // large uploads
  c.open_fn          = sockOpen;   // TCP_NODELAY
  c.send_wait_timeout = 5;

  if (httpd_start(&webHttpd, &c) == ESP_OK) {
    reg(webHttpd, "/",           h_index);
    reg(webHttpd, "/ui-art/*",   h_ui_art);
    reg(webHttpd, "/jpg",        h_jpg);
    reg(webHttpd, "/photo",      h_photo);
    reg(webHttpd, "/log",        h_log);
    reg(webHttpd, "/dl",         h_dl);
    reg(webHttpd, "/info",       h_info);
    reg(webHttpd, "/net",        h_net);
    reg(webHttpd, "/net/scan",   h_net_scan);
    reg(webHttpd, "/ota/check",  h_ota_check);
    reg(webHttpd, "/ota/apply",  h_ota_apply);
    reg(webHttpd, "/sync",       h_sync);
    reg(webHttpd, "/shot",       h_shot);
    reg(webHttpd, "/gallery",    h_gallery);
    reg(webHttpd, "/frame",      h_frame);
    reg(webHttpd, "/files",      h_files);
    regPost(webHttpd, "/upload", h_upload);
    reg(webHttpd, "/hotspot-detect.html",   h_probe);   // iOS
    reg(webHttpd, "/library/test/success.html", h_probe); // iOS
    reg(webHttpd, "/generate_204",          h_probe);     // Android
    reg(webHttpd, "/gen_204",               h_probe);
    reg(webHttpd, "/connecttest.txt",       h_probe);     // Windows
    reg(webHttpd, "/ncsi.txt",              h_probe);
    reg(webHttpd, "/status",     h_status);
    reg(webHttpd, "/ap",         h_ap);
    regPost(webHttpd, "/ap/pass", h_ap_pass);
    reg(webHttpd, "/capture",    h_capture);
#ifdef EXHIBITION_MODE
    reg(webHttpd, "/room",         h_room);
    reg(webHttpd, "/archive",      h_archive);
    reg(webHttpd, "/archive/list", h_arch_list);
    reg(webHttpd, "/archive/img",  h_arch_img);
    regPost(webHttpd, "/view",     h_view);
    reg(webHttpd, "/session/new",  h_session_new);
#endif
    reg(webHttpd, "/name",       h_devname);
#ifdef WUW_PANEL
    reg(webHttpd, "/panel",      h_panel);
    reg(webHttpd, "/panel/skin", h_panel_skin);
    reg(webHttpd, "/panel/bwo-wall", h_panel_bwo_wall);
#endif
    reg(webHttpd, "/presets",    h_preset_list);
    reg(webHttpd, "/preset",     h_preset_load);
    reg(webHttpd, "/preset/del", h_preset_del);
    regPost(webHttpd, "/preset", h_preset_save);
    reg(webHttpd, "/note",       h_note_get);
    regPost(webHttpd, "/note",   h_note);
#ifdef WUW_LINK
    reg(webHttpd, "/link/peers",  h_link_peers);
    reg(webHttpd, "/link/fire",   h_link_fire);
    reg(webHttpd, "/link/pull",   h_link_pull);
    reg(webHttpd, "/link/config", h_link_config);
#endif
    reg(webHttpd, "/preroll",      h_preroll);
    reg(webHttpd, "/preroll/save", h_preroll_save);
    reg(webHttpd, "/tasks",      h_tasks);
    reg(webHttpd, "/night",      h_night);
    reg(webHttpd, "/set",        h_set);
    reg(webHttpd, "/rec/start",  h_rec_start);
    reg(webHttpd, "/rec/stop",   h_rec_stop);
    reg(webHttpd, "/rec/config", h_rec_config);
    reg(webHttpd, "/rec/stats",  h_rec_stats);
    // Genuinely last: esp_http_server returns the FIRST matching handler, so
    // anything registered after this wildcard becomes unreachable.
    reg(webHttpd, "/*",          h_portal);
    Serial.printf("[WEB] :80 OK — %d routes, catch-all registered last\n", nReg);
    if (wildIdx != nReg - 1)
      Serial.printf("[WEB] *** ROUTING BUG: /* is #%d of %d, %d routes unreachable ***\n",
                    wildIdx + 1, nReg, nReg - 1 - wildIdx);
  } else {
    Serial.println("[WEB] :80 FAILED");
  }

  httpd_config_t s = HTTPD_DEFAULT_CONFIG();
  s.server_port      = 81;
  s.ctrl_port        = 32769;
  s.core_id          = 1;
  s.task_priority    = 4;
  s.stack_size       = 6144;
  s.max_uri_handlers = 4;
  s.max_open_sockets = 2;
  s.lru_purge_enable = true;
  s.open_fn          = sockOpen;   // TCP_NODELAY

  if (httpd_start(&streamHttpd, &s) == ESP_OK) {
    reg(streamHttpd, "/stream", h_stream);
    reg(streamHttpd, "/sd",     h_sdfile);
    Serial.println("[WEB] :81/stream OK");
  } else {
    Serial.println("[WEB] :81 FAILED");
  }
}

/* ── ISO and exposure, mapped onto the sensor that is actually fitted ───────
 * Measured on the OV5640 by reading its own registers back after each write:
 *
 *   gain:     agc_gain g -> register 16g-1, i.e. the value IS the gain
 *             multiplier, 1x at g=1 up to 30x at g=30. Linear, not logarithmic.
 *   exposure: aec_value v -> exposure of v sensor lines, clamped at the frame
 *             height (744 lines in the default mode).
 *
 * So ISO 100/200/400/800/1600 are exactly 1x/2x/4x/8x/16x, and the top stop is
 * 30x -- about ISO 3000, labelled as such rather than 3200. Exposure is turned
 * into a shutter fraction using the line time of the mode the sensor is in (see
 * lightReadMode), re-derived whenever the stream resolution changes. The
 * labels still carry a "~": the clock figures are fitted, not from a datasheet.
 *
 * All of it is derived at boot from register read-back rather than hard-coded,
 * so a different sensor (the OV3660 in CAM 02) gets its own table. If the
 * registers will not read back, it falls back to the linear assumption and
 * shows plain step numbers instead of invented shutter speeds. */
#define LIGHT_ISO_STOPS 7
#define LIGHT_EXP_STOPS 11
static uint8_t  lightIsoGain[LIGHT_ISO_STOPS]   = { 0, 1, 2, 4, 8, 16, 30 };
static uint16_t lightExpValue[LIGHT_EXP_STOPS]  = { 0, 2, 4, 8, 16, 32, 64, 128, 256, 512, 740 };
static char     lightExpName[LIGHT_EXP_STOPS][10] = { "AUTO" };
static bool     lightMeasured = false;
/* The exposure ladder is in sensor LINES and does not depend on the mode. What
   a line is worth in time does: the OV5640 clocks its modes differently
   (45 us in SVGA, 103 us in SXGA), so the shutter LABELS are re-derived whenever
   the stream resolution changes. */
static const uint16_t lightExpLines[LIGHT_EXP_STOPS] = { 0, 2, 4, 8, 16, 32, 64, 128, 256, 512, 740 };
static bool     lightExpOk   = false;
static float    lightLineUs  = 0;
static int      lightVts     = 0;

static int lightRegPair(sensor_t* s, int hiReg, int hiMask, int loReg) {
  int hi = s->get_reg(s, hiReg, hiMask), lo = s->get_reg(s, loReg, 0xFF);
  return (hi < 0 || lo < 0) ? -1 : (hi << 8) | lo;
}

/* Shutter labels from the line time of the mode the sensor is in right now. A
   stop longer than one frame is clamped by the sensor to the frame height, so
   it is named for what it really does. */
static void lightRelabel() {
  if (!lightExpOk || lightLineUs <= 0) return;
  for (int i = 1; i < LIGHT_EXP_STOPS; ++i) {
    int lines = lightExpLines[i];
    if (lightVts > 8 && lines > lightVts - 4) lines = lightVts - 4;
    float shutter = lines * lightLineUs / 1e6f;
    /* Two significant figures, not the nearest "standard" shutter speed. The
       ladder doubles each stop, and the last two stops (about 1/37 and 1/25)
       both snapped to 1/30 -- two steps with the same name. */
    float n = 1.0f / shutter;
    float mag = powf(10.0f, floorf(log10f(n)) - 1.0f);
    int shown = (int)(roundf(n / mag) * mag);
    snprintf(lightExpName[i], sizeof(lightExpName[i]), "~1/%d", shown);
  }
}

/* What one sensor line is worth in time, from the sensor's own registers.
 *
 * line = HTS / (the sensor core's pixel clock). The OV5640 runs that clock at
 * about 55 MHz in its subsampled modes and about 27 MHz when it reads the full
 * array (register 0x3814 tells them apart: 0x31 subsampled, 0x11 full). Measured
 * on CAM 02 at 24 MHz XCLK by timing real frames in all eight modes from QVGA to
 * 1080p, before anything else touched the camera:
 *
 *     mode     HTS   0x3814   frame      line     HTS/line
 *     SVGA    2060     31     36.8 ms    37.4 us   55.1 MHz
 *     XGA     2644     31     47.3 ms    48.1 us   55.0
 *     HD      2644     31     35.9 ms    48.2 us   54.8
 *     SXGA    2684     11    193.3 ms    98.2 us   27.3
 *     UXGA    2844     11    206.4 ms   104.9 us   27.1
 *     FHD     2844     11    156.9 ms   105.5 us   27.0
 *
 * which is also why SXGA and up run at 5-6 fps: same line count, half the clock.
 * Reading registers instead of timing frames means this works at any moment
 * (the panel's viewfinder holds the frame buffer while it draws, which makes
 * frame timing meaningless once the OS is up) and costs two I2C reads.
 * Other sensors are timed once at boot and scaled by HTS afterwards. */
#define LIGHT_SCLK_SUBSAMPLED_MHZ 55.0f
#define LIGHT_SCLK_FULLARRAY_MHZ  27.1f
static float lightBootLineUs = 0;
static int   lightBootHts    = 0;

static bool lightReadMode() {
  sensor_t* s = esp_camera_sensor_get();
  if (!s || !s->get_reg || !lightExpOk) return false;
  int vts = lightRegPair(s, 0x380E, 0xFF, 0x380F);
  int hts = lightRegPair(s, 0x380C, 0xFF, 0x380D);
  int inc = s->get_reg(s, 0x3814, 0xFF);
  if (vts <= 100 || hts <= 100) return false;
  float us;
  if (camSensorPid == 0x5640 && inc >= 0) {
    us = hts / (((inc & 0xF0) == 0x30) ? LIGHT_SCLK_SUBSAMPLED_MHZ : LIGHT_SCLK_FULLARRAY_MHZ);
  } else if (lightBootLineUs > 0 && lightBootHts > 0) {
    us = lightBootLineUs * hts / lightBootHts;
  } else return false;
  lightVts = vts; lightLineUs = us;
  lightRelabel();
  return true;
}

/* Frame period of the mode the sensor is in, for sensors whose clock tree is not
   known. Only meaningful with nothing else using the camera, i.e. at boot. */
static uint32_t lightTimeFrames(int frames) {
  uint64_t prev = 0; uint32_t best = 0xFFFFFFFFu;
  for (int i = 0; i < frames; ++i) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) continue;
    uint64_t t = (uint64_t)fb->timestamp.tv_sec * 1000000ull + (uint64_t)fb->timestamp.tv_usec;
    if (!t) t = (uint64_t)esp_timer_get_time();
    esp_camera_fb_return(fb);
    if (prev && t > prev && i >= 3) {                  // the first frames can be stale
      uint32_t d = (uint32_t)(t - prev);
      if (d > 5000 && d < best) best = d;
    }
    prev = t;
  }
  return best == 0xFFFFFFFFu ? 0 : best;
}

static void lightModeChanged() { lightReadMode(); }

static void lightCalibrate() {
  for (int i = 1; i < LIGHT_EXP_STOPS; ++i) snprintf(lightExpName[i], sizeof(lightExpName[i]), "M%d", i);
  sensor_t* s = esp_camera_sensor_get();
  if (!s || !s->get_reg) { Serial.println("[LIGHT] no register access - using defaults"); return; }

  // ---- gain: find the agc_gain nearest each ISO multiplier ----
  float real[31] = {};
  s->set_gain_ctrl(s, 0);
  bool gainOk = true;
  for (int g = 1; g <= 30; ++g) {
    s->set_agc_gain(s, g); delay(6);
    int raw = lightRegPair(s, 0x350A, 0x03, 0x350B);
    if (raw < 0) { gainOk = false; break; }
    real[g] = (raw + 1) / 16.0f;
  }
  bool monotonic = gainOk;
  for (int g = 2; g <= 30 && monotonic; ++g) if (real[g] < real[g - 1]) monotonic = false;
  if (gainOk && monotonic) {
    static const float target[LIGHT_ISO_STOPS] = { 0, 1, 2, 4, 8, 16, 32 };
    for (int i = 1; i < LIGHT_ISO_STOPS; ++i) {
      int best = 1;
      for (int g = 2; g <= 30; ++g)
        if (fabsf(real[g] - target[i]) < fabsf(real[best] - target[i])) best = g;
      lightIsoGain[i] = (uint8_t)best;
    }
  } else Serial.println("[LIGHT] gain registers unreadable or not monotonic - linear fallback");

  // ---- exposure: how many sensor lines does one aec_value step buy? ----
  s->set_exposure_ctrl(s, 0);
  s->set_aec_value(s, 64); delay(6);
  int hi = s->get_reg(s, 0x3500, 0x0F), mid = s->get_reg(s, 0x3501, 0xFF), lo = s->get_reg(s, 0x3502, 0xFF);
  int vts = lightRegPair(s, 0x380E, 0xFF, 0x380F);
  float linesPerValue = 1.0f; bool expOk = false;
  if (hi >= 0 && mid >= 0 && lo >= 0 && vts > 100) {
    float lines64 = (((hi << 16) | (mid << 8) | lo)) / 16.0f;
    if (lines64 > 8 && lines64 < 4096) { linesPerValue = lines64 / 64.0f; expOk = true; }
  }

  // ---- the ladder, in lines: mode-independent, scaled to this sensor's value units ----
  for (int i = 1; i < LIGHT_EXP_STOPS; ++i)
    if (expOk) lightExpValue[i] = (uint16_t)constrain((int)(lightExpLines[i] / linesPerValue + 0.5f), 1, 1200);
  lightExpOk = expOk;

  // ---- shutter speed: needs the time one line takes, from the frame period ----
  if (expOk) {
    if (camSensorPid != 0x5640) {
      s->set_exposure_ctrl(s, 1); s->set_gain_ctrl(s, 1);
      uint32_t period = lightTimeFrames(12);
      int vts0 = lightRegPair(s, 0x380E, 0xFF, 0x380F), hts0 = lightRegPair(s, 0x380C, 0xFF, 0x380D);
      if (period && vts0 > 100 && hts0 > 100) { lightBootLineUs = (float)period / vts0; lightBootHts = hts0; }
    }
    lightReadMode();
  }
  s->set_gain_ctrl(s, 1); s->set_exposure_ctrl(s, 1);
  lightMeasured = gainOk && expOk;
  Serial.printf("[LIGHT] %s | mode %u q%d, sensor limit %.1f fps | VTS %d, line %.1f us | ISO gains %u %u %u %u %u %u | shutter %s %s %s ... %s\n",
    lightMeasured ? "measured" : "partly measured", (unsigned)streamRes, streamQ,
    lightLineUs > 0 && lightVts > 0 ? 1e6f / (lightLineUs * lightVts) : 0.0f, lightVts, lightLineUs,
    lightIsoGain[1], lightIsoGain[2], lightIsoGain[3], lightIsoGain[4], lightIsoGain[5], lightIsoGain[6],
    lightExpName[1], lightExpName[2], lightExpName[3], lightExpName[LIGHT_EXP_STOPS - 1]);
}

// ═══════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════

/* ── Shutter button, GPIO 14 ──────────────────────────────────────────────
   Moved here from GPIO 1 when the panel was laid out. GPIO 1 sits inside the
   run of four adjacent header pins the display ribbon now uses, and a button
   is the one signal that does not care where it lives: it is mechanical,
   debounced in software, and carries no edge faster than a thumb. Giving up
   its pin to keep the 40 MHz harness contiguous is a trade with no downside.

   GPIO 14 is on the OTHER header, at the bottom, next to a ground pin -- so
   the switch is a two-wire connector to adjacent pins, nowhere near the
   display ribbon. It is a plain GPIO: no strapping role, nothing happens on
   it at boot, and the camera does not use it.

   Wire ONE leg of the 12x12 tactile switch
   here and the DIAGONALLY OPPOSITE leg to GND. The four legs are two
   internally-joined pairs; diagonal legs are always in different pairs
   whichever way the switch is turned, so that choice is orientation-proof.
   Same-side legs are a permanent short and read as "held down forever".

     short press          -> photo
     hold 3 s             -> start recording, fired AT the 3 s mark
     any press while rec  -> stop

   A core-0 task debounces and queues actions. A core-1 worker performs the
   capture through shutterMux, the same serialized path used by HTTP and the
   panel, so SD latency cannot swallow a button edge or block loop(). */
#define BTN_PIN      14
#define BTN_DEBOUNCE  25      // ms of steady level before an edge counts
#define BTN_HOLD_MS   3000    // hold this long to begin recording

enum BtnAct : uint8_t { ACT_SHOOT = 1, ACT_REC_START, ACT_REC_STOP };
static QueueHandle_t btnQ = nullptr;

static void btnTask(void*) {
  pinMode(BTN_PIN, INPUT_PULLUP);
  bool     raw = false, down = false;
  uint32_t lastEdge = 0, downAt = 0;
  bool     holdFired = false;    // this press already started a recording
  bool     needRelease = false;  // swallow input until the button comes up

  for (;;) {
    bool now = (digitalRead(BTN_PIN) == LOW);   // pull-up: pressed reads LOW
    uint32_t t = millis();
    if (now != raw) { raw = now; lastEdge = t; }        // bounce restarts the clock

    if ((t - lastEdge) >= BTN_DEBOUNCE && now != down) {
      down = now;
      if (down) {                                       // ── press ──
        downAt = t; holdFired = false;
        if (!needRelease && rec_is_active()) {
          BtnAct a = ACT_REC_STOP;
          xQueueSend(btnQ, &a, 0);
          needRelease = true;
        }
      } else {                                          // ── release ──
        if (needRelease)            needRelease = false;
        else if (!holdFired && !rec_is_active()) {
          BtnAct a = ACT_SHOOT;                         // it never became a hold
          xQueueSend(btnQ, &a, 0);
        }
        holdFired = false;
      }
    }

    /* Held past the threshold while idle: start now, while their finger is
       still down, so the feedback lands under the press. Then latch, because
       the release that follows would otherwise read as the stop press and
       kill the clip the instant it started. */
    if (down && !holdFired && !needRelease && !rec_is_active() &&
        (t - downAt) >= BTN_HOLD_MS) {
      BtnAct a = ACT_REC_START;
      xQueueSend(btnQ, &a, 0);
      holdFired = true;
      needRelease = true;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

/* ── The button's action, on its own task ─────────────────────────────────
 * Capture can take hundreds of milliseconds, so it must not run in loop(),
 * which owns captive-portal DNS and network housekeeping.
 * Priority 2 -- above loop() so a press is not starved, below the web server
 * so a room full of phones still gets served first. The mutex carries
 * priority inheritance, so a low-priority capture holding it cannot block a
 * high-priority web request indefinitely. */
static void shotTask(void*) {
  BtnAct a;
  for (;;) {
    if (xQueueReceive(btnQ, &a, portMAX_DELAY) != pdTRUE) continue;
    if (a == ACT_SHOOT) {
      ShotResult r = shutterTake(devName);   // same guarded path as the web
      Serial.printf("[BTN] photo -> %s\n", r.detail.c_str());
    } else if (a == ACT_REC_START) {
      bool ok = shutterRecStart();
      Serial.printf("[BTN] rec start -> %s\n", ok ? "OK" : "refused");
    } else if (a == ACT_REC_STOP) {
      String r = shutterRecStop();
      Serial.printf("[BTN] rec stop -> %s\n", r.c_str());
    }
  }
}


#ifdef WUW_PANEL
/* ── what the panel is allowed to do ──────────────────────────────────────
   The screen never touches the camera itself. Every action goes through the
   same serialised shutter the web handlers use, so a tap on the glass and a
   tap on a phone can never both be inside the sensor at once. */
static bool panelShootHook(const char* who) {
  ShotResult r = shutterTake(who && *who ? who : devName);
  return r.ok;
}
static bool panelRecHook(bool start) {
  if (start) return shutterRecStart();
  shutterRecStop();
  return false;
}
static bool panelCollectiveHook(const char* tag) {
  bool ok = linkFire(500, tag && *tag ? tag : "collective");
  if (!ok || !sdReady) return ok;
  static char peers[1024];
  size_t peerLen = linkPeersJson(peers, sizeof(peers));
  if (!peerLen) peerLen = snprintf(peers, sizeof(peers), "{\"peers\":[]}");
  char path[96];
#ifdef EXHIBITION_MODE
  snprintf(path, sizeof(path), "%s/COLLECTIVE_%08lu.json", pasarSessionDir(),
           (unsigned long)millis());
#else
  snprintf(path, sizeof(path), "/COLLECTIVE_%08lu.json", (unsigned long)millis());
#endif
  File f = SD_MMC.open(path, FILE_WRITE);
  if (f) {
    f.printf("{\"session\":\"%s\",\"initiator\":\"%s\",\"tag\":\"%.15s\",\"link\":",
#ifdef EXHIBITION_MODE
             pasarSession(),
#else
             "standalone",
#endif
             devName, tag && *tag ? tag : "collective");
    f.write((const uint8_t*)peers, peerLen);
    f.print("}");
    f.close();
  }
  return true;
}
/* ── Upstream WiFi for the on-device OS ─────────────────────────────────────
 * Thin wrappers over the shared sta* functions above. The one thing added here
 * is the reason a join failed: WiFi.status() collapses "wrong password" and
 * "network not found" into the same disconnected state, and on a touchscreen
 * with a pop-up keyboard those need different messages -- one means retype the
 * password, the other means move closer. The radio does report it, through the
 * disconnect event. */
static volatile uint8_t staReason = 0;        // 0 none, 1 wrong password, 2 not found, 3 other
static void staEvent(arduino_event_id_t ev_id, arduino_event_info_t info) {
  if (ev_id == ARDUINO_EVENT_WIFI_STA_GOT_IP) { staReason = 0; return; }
  if (ev_id != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  switch (info.wifi_sta_disconnected.reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
      staReason = 1; break;
    case WIFI_REASON_NO_AP_FOUND:
      staReason = 2; break;
    default:
      if (staReason == 0) staReason = 3;       // never overwrite a more specific cause
  }
}
static void panelWifiScanStart() { WiFi.mode(WIFI_AP_STA); WiFi.scanNetworks(true, true); }
static int panelWifiScanResults(PanelWifiNet* out, int cap) {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  if (n == WIFI_SCAN_FAILED || n < 0) return 0;
  int count = 0;
  for (int i = 0; i < n && count < cap; ++i) {           // arrives strongest-first
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;                        // hidden: typed in by hand
    bool dup = false;
    for (int k = 0; k < count; ++k) if (!strcmp(out[k].ssid, ssid.c_str())) { dup = true; break; }
    if (dup) continue;                                   // several radios, one network
    snprintf(out[count].ssid, sizeof(out[count].ssid), "%s", ssid.c_str());
    out[count].rssi = (int8_t)constrain(WiFi.RSSI(i), -127, 0);
    out[count].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    ++count;
  }
  WiFi.scanDelete();
  return count;
}
static void panelWifiJoin(const char* ssid, const char* pass) { staReason = 0; staJoin(ssid, pass); }
static void panelWifiForget() { staReason = 0; staForget(); }
static void panelWifiState(PanelWifiState* st) {
  memset(st, 0, sizeof(*st));
  st->connected = WiFi.status() == WL_CONNECTED;
  snprintf(st->ssid, sizeof(st->ssid), "%s", st->connected ? WiFi.SSID().c_str() : staSsid.c_str());
  if (st->connected) {
    st->rssi = (int8_t)constrain(WiFi.RSSI(), -127, 0);
    snprintf(st->ip, sizeof(st->ip), "%s", WiFi.localIP().toString().c_str());
  }
  st->configured = staSsid.length() > 0;
  st->connecting = st->configured && !st->connected && staReason == 0;
  st->reason = st->connected ? 0 : staReason;
}

/* What each stop is called, asked for by the on-screen controls. */
static const char* panelLightLabel(PanelCameraControl control, int index) {
  static const char* const iso[LIGHT_ISO_STOPS] = { "AUTO", "100", "200", "400", "800", "1600", "3000" };
  if (control == PANEL_CAM_ISO) return iso[constrain(index, 0, LIGHT_ISO_STOPS - 1)];
  if (control == PANEL_CAM_EXPOSURE) return lightExpName[constrain(index, 0, LIGHT_EXP_STOPS - 1)];
  return "";
}

static bool panelCameraHook(PanelCameraControl control, int value) {
  if (!camOk) return false;
  sensor_t* s = esp_camera_sensor_get();
  if (!s) return false;
  switch (control) {
    case PANEL_CAM_STREAM_RES:
      if (rec_is_active() || !validFrameSize(value)) return false;
      streamRes = value; s->set_framesize(s, (framesize_t)value);
      prefs.putUChar("streamres", streamRes); lightModeChanged(); break;
    case PANEL_CAM_PHOTO_RES:
      if (!validFrameSize(value)) return false;
      photoRes = value; prefs.putUChar("photores", photoRes); break;
    case PANEL_CAM_STREAM_QUALITY:
      streamQ = constrain(value, 8, 45); s->set_quality(s, streamQ);
      prefs.putInt("streamq", streamQ); break;
    case PANEL_CAM_PHOTO_QUALITY:
      photoQ = constrain(value, 4, 20); prefs.putInt("photoq", photoQ); break;
    case PANEL_CAM_BRIGHTNESS:
      camBrightness = constrain(value, -2, 2); s->set_brightness(s, camBrightness);
      prefs.putInt("bright", camBrightness); break;
    case PANEL_CAM_CONTRAST:
      s->set_contrast(s, constrain(value, -2, 2)); break;
    case PANEL_CAM_AE_LEVEL:
      s->set_ae_level(s, constrain(value, -2, 2)); break;
    case PANEL_CAM_HMIRROR:
      camHMirror = value != 0; s->set_hmirror(s, camHMirror);
      prefs.putBool("hmirror", camHMirror); break;
    case PANEL_CAM_VFLIP:
      camVFlip = value != 0; s->set_vflip(s, camVFlip);
      prefs.putBool("vflip", camVFlip); break;
    case PANEL_CAM_ISO:                 // 0 = auto, else a measured gain
      if (value <= 0) s->set_gain_ctrl(s, 1);
      else { s->set_gain_ctrl(s, 0); s->set_agc_gain(s, lightIsoGain[constrain(value, 1, LIGHT_ISO_STOPS - 1)]); }
      break;
    case PANEL_CAM_EXPOSURE:            // 0 = auto, else a measured exposure
      if (value <= 0) s->set_exposure_ctrl(s, 1);
      else { s->set_exposure_ctrl(s, 0); s->set_aec_value(s, lightExpValue[constrain(value, 1, LIGHT_EXP_STOPS - 1)]); }
      break;
    case PANEL_CAM_AEC:
      s->set_exposure_ctrl(s, value != 0); break;
    case PANEL_CAM_AEC_VALUE:
      s->set_aec_value(s, constrain(value, 0, 1200)); break;
    case PANEL_CAM_AGC:
      s->set_gain_ctrl(s, value != 0); break;
    case PANEL_CAM_AGC_GAIN:
      s->set_agc_gain(s, constrain(value, 0, 30)); break;
    default: return false;
  }
  return true;
}

/* Pushed once a tick. The panel only reads this, so the two sides never
   contend over state. */
static void panelFeed() {
  if (!panelPresent()) return;
  PanelStatus s = {};
  s.cam = camOk;
  s.sd  = sdReady;
  s.net = WiFi.status() == WL_CONNECTED;
  s.peers = (uint8_t)min(linkPeerCount(), 255);
#ifdef EXHIBITION_MODE
  s.visitors  = pasarCount();
  s.photos    = pasarPhotoCount();
  s.lastImage = pasarLastImage();
  s.session   = pasarSession();
#else
  s.visitors = 0; s.photos = (uint32_t)photoCount; s.lastImage = 0;
  s.session = "-";
#endif
  s.device = devName;
  s.streamRes = streamRes;
  s.photoRes = photoRes;
  s.maxRes = camMaxRes;
  s.streamQuality = streamQ;
  s.photoQuality = photoQ;
  s.brightness = camBrightness;
  if (camOk) {
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor) {
      s.contrast = sensor->status.contrast;
      s.aeLevel = sensor->status.ae_level;
    }
  }
  s.hmirror = camHMirror;
  s.vflip = camVFlip;
  panelPublish(s);
}
#endif

/* What the other cameras see of us. Same shape as the panel's feed and for
   the same reason: the mesh reads a published copy instead of reaching into
   our state, so the link task and the camera never contend. */
#ifdef WUW_LINK
/* A synchronised shutter from another camera lands here. It goes through
   exactly the same mutex, minimum gap and SD path as the button on the case,
   so a peer cannot reach the sensor by a route the local user cannot. */
static bool linkShootHook(const char* who) {
  ShotResult r = shutterTake(who && *who ? who : "link");
  return r.ok;
}

static void linkFeed() {
  LinkSelf s;
  s.name = devName;
  s.cam  = camOk;
  s.sd   = sdReady;
#ifdef EXHIBITION_MODE
  s.session = pasarSession();
  s.photos  = pasarPhotoCount();
#else
  s.session = "-";
  s.photos  = (uint32_t)photoCount;
#endif
  linkPublish(s);
}
#else
static void linkFeed() {}          /* LinkSelf does not exist in this build */
#endif

/* Holding the shutter button while the camera powers on for 8 seconds restores the factory WiFi
   password, for an owner who has forgotten theirs. Nothing happens if the button is up at power-on
   or released early. (A button wired permanently shorted would reset it on every boot, which is why
   the log says so loudly.) */
static void apRecoveryCheck() {
  pinMode(BTN_PIN, INPUT_PULLUP);
  delay(30);
  if (digitalRead(BTN_PIN) != LOW) return;
  Serial.println("[AP] button held at power-on - keep holding 8 s to restore the factory WiFi password");
  const uint32_t t0 = millis();
  while (digitalRead(BTN_PIN) == LOW) {
    if (millis() - t0 >= 8000) {
      prefs.remove("appass");
      Serial.println("[AP] factory WiFi password restored - release the button");
      while (digitalRead(BTN_PIN) == LOW) delay(20);   // or the button task would read the hold as 'record'
      return;
    }
    delay(20);
  }
}

/* A changed password goes live here, a moment after the page was told, so the reply reaches the
   phone before the radio drops it. Keeps the channel the radio is on (it follows the upstream
   router while a station link is up). */
static void apTick() {
  if (!apApplyAt || (int32_t)(millis() - apApplyAt) < 0) return;
  apApplyAt = 0;
  int ch = WiFi.channel();
  if (ch < 1 || ch > 13) ch = 1;
  WiFi.softAP(apName, apPass, ch, 0, 4);
  Serial.println("[WiFi] access point restarted with the new password");
}

void setup() {
  Serial.begin(115200);      // native USB CDC — baud value is cosmetic
  delay(1000);               // give the host a moment; never block on !Serial
  setCpuFrequencyMhz(240);
  Serial.printf("\n== %s  fw %s ==\n", FW_MODEL, FW_VERSION);
  devNameLoad();
  bootGuardBegin();

  /* Allocate shared capture controls before any web or panel task can call
     into the shutter. GPIO 14 is only the physical button; touch polls Z1. */
  shutterMux = xSemaphoreCreateMutex();
  if (!shutterMux) Serial.println("[SHUTTER] mutex alloc FAILED - captures disabled");
  btnQ = xQueueCreate(4, sizeof(BtnAct));
  if (!btnQ) Serial.println("[BTN] queue alloc failed - shutter disabled");


  // Never hard-loop on camera failure — AP + UI must always come up
  camOk = initCamera();
  if (camOk) lightCalibrate();
  if (!camOk)
    Serial.println("[CAM] running WITHOUT camera — check ribbon seating");

  sdReady = initSD();
  printf("[BOOT] %s\n", "SD done");
  delay(200);

  // ── WiFi AP ──
  WiFi.mode(WIFI_AP);
  {
    uint8_t m[6]; WiFi.softAPmacAddress(m);
#ifdef UNIQUE_SSID
    snprintf(apName, sizeof(apName), "%s-%02X%02X", AP_SSID, m[4], m[5]);
#else
    snprintf(apName, sizeof(apName), "%s", AP_SSID);
#endif
  }
  apRecoveryCheck();
  apPassLoad();
  WiFi.softAP(apName, apPass, 1, 0, 4);
  Serial.printf("[WiFi] access point password: %s\n", apPassCustom ? "custom (set by the owner)" : "FACTORY default - set your own on the page");
  delay(300);
  WiFi.setSleep(false);              // no DTIM sleep → no latency spikes
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(68);     // 17 dBm

  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[WiFi] AP %s | %s\n", apName, ip.toString().c_str());

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", ip);            // every hostname resolves here
  if (MDNS.begin("wuwcam")) {              // http://wuwcam.local
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mDNS] http://wuwcam.local");
  }

  startServers();
  printf("[BOOT] %s\n", "servers up");

  Serial.println("[READY]");
  Serial.printf("  UI:     http://%s\n", ip.toString().c_str());
  Serial.printf("  MJPEG:  http://%s:81/stream\n", ip.toString().c_str());
  Serial.printf("  CAM:    %s | SD: %s\n", camOk ? "OK" : "OFFLINE",
                sdReady ? "OK" : "NOT FOUND");
  Serial.printf("  CPU:    %lu MHz | Heap: %lu | PSRAM: %lu\n",
                (unsigned long)getCpuFrequencyMhz(),
                (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getFreePsram());

  if (btnQ && shutterMux) {
    xTaskCreatePinnedToCore(btnTask,  "btn",  3072, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(shotTask, "shot", 6144, nullptr, 2, nullptr, 1);
    Serial.printf("[BTN] shutter on GPIO %d (core 0) - tap=photo, hold %.0fs=record\n",
                  BTN_PIN, BTN_HOLD_MS / 1000.0);
  }
#ifdef WUW_PANEL
  /* Probed, not assumed. No panel attached is a normal configuration, not a
     failure -- the camera simply runs headless and every panel call is inert. */
  printf("[BOOT] reaching panelBegin\n");
  panelHooks(panelShootHook, panelRecHook, panelCameraHook, panelCollectiveHook,
             panelLightLabel);
  { PanelWifiApi w = { panelWifiScanStart, panelWifiScanResults, panelWifiJoin,
                       panelWifiForget, panelWifiState };
    panelWifiHooks(w); }
  WiFi.onEvent(staEvent);
  if (panelBegin()) {
    panelFeed();
    Serial.println("[PANEL] device OS running on the glass");
  } else Serial.println("[PANEL] headless - camera unaffected");
#endif
#ifdef EXHIBITION_MODE
  pasarBegin();
  Serial.printf("[PASAR] WUW-01 / PASAR PROTOCOL - session %s\n", pasarSession());
  wuwBridgeBegin();
  wuwBridgeBoot();
#endif
#ifdef WUW_LINK
  linkHooks(linkShootHook);
  linkBegin();
  linkFeed();                     // beacon with real values from the first one
#endif
  bootGuardOK();                  // this build reaches READY — mark it good
  if (staSsid.length()) staConnect(6000);   // optional internet, AP stays up
}

// ═══════════════════════════════════════════════════════════════════════
//  LOOP — everything is task-based; just report health
// ═══════════════════════════════════════════════════════════════════════
void loop() {
  /* the shutter has its own task now -- loop() must never block on SD */
  wuwBridgeTick();       // emits IDLE after a long quiet spell
  linkFeed();            // a struct copy; keeps the beacon's counts live
  staTick();             // keeps the upstream WiFi link truthful and alive
  apTick();              // applies a new access-point password a moment after the page was told
  dnsServer.processNextRequest();          // captive portal DNS
  /* A clip that ended by itself (card full, writer error) never passes through shutterRecStop, so
     hand its memory back here. The shutter lock keeps this from racing a stop that is under way. */
  if (recModeOn && !rec_is_active() && shutterMux && xSemaphoreTake(shutterMux, 0) == pdTRUE) {
    recModeExit();
    xSemaphoreGive(shutterMux);
  }
#ifdef WUW_PANEL
  static uint32_t lastPanel = 0;
  if (millis() - lastPanel > 1000) { lastPanel = millis(); panelFeed(); }
#endif
  static uint32_t last = 0;
  if (millis() - last > 30000) {
    last = millis();
    {
      /* The tightest stack in the system, named. One number in the log that
         says whether anything is heading for an overflow. */
      static const char* tn[] = { "loopTask", "httpd", "stream_httpd", "shot",
                                  "btn", "panel", "link", "lpull", "rec" };
      const char* worstName = "-";
      unsigned worstFree = 0xFFFFFFFF;
      for (size_t i = 0; i < sizeof(tn) / sizeof(tn[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(tn[i]);
        if (!h) continue;
        unsigned f = (unsigned)uxTaskGetStackHighWaterMark(h);
        if (f < worstFree) { worstFree = f; worstName = tn[i]; }
      }
      Serial.printf("[HEALTH] heap=%lu psram=%lu clients=%d rec=%d stack:%s=%uB\n",
                    (unsigned long)ESP.getFreeHeap(),
                    (unsigned long)ESP.getFreePsram(),
                    WiFi.softAPgetStationNum(), rec_is_active(),
                    worstName, worstFree == 0xFFFFFFFF ? 0 : worstFree);
      if (worstFree < 512)
        ev("stack low: %s has %uB free", worstName, worstFree);
    }
#ifdef EXHIBITION_MODE
    /* The things that actually end a six-hour exhibition, checked once a
       tick. Nothing here is clever; it is all recovery. */
    {
      static uint8_t camFails = 0;
      if (sdReady && !SD_MMC.cardType()) {          // card pulled or died
        sdReady = false;
        ev("SD vanished - captures refused, stream continues");
        Serial.println("[HEALTH] SD lost");
      } else if (!sdReady) {                        // and try to get it back
        if (initSD()) { sdReady = true; archStamp = -1; scanExisting();
                        ev("SD returned"); Serial.println("[HEALTH] SD remounted"); }
      }
      if (sdReady) {
        uint64_t total = SD_MMC.totalBytes(), used = SD_MMC.usedBytes();
        if (total && (total - used) < (8ULL * 1024 * 1024))
          ev("SD nearly full: %u MB left", (unsigned)((total - used) / 1048576));
      }
      if (camOk && !esp_camera_sensor_get()) {
        if (++camFails >= 3) { ev("camera unresponsive x3"); camFails = 0; }
      } else camFails = 0;
      if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 40000)
        ev("internal heap low: %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
#endif
    evDump();
  }
  delay(5);
}
