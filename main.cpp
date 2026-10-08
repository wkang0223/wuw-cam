/*
 * WUW-CAM — ESP32-CAM OV3660 Firmware
 * ─────────────────────────────────────────────────────────────────
 *  WiFi AP   : wuw / wuwuwuwu
 *  Web UI    : http://192.168.4.1        (port 80)
 *  MJPEG     : http://192.168.4.1:81/stream   (desktop bonus)
 *  Photo     : QXGA 2048×1536 → SD card as /IMG_xxxx.jpg
 *  SD Format : FAT32 (1-bit MMC mode — avoids GPIO4/LED conflict)
 *
 *  Web stack: ESP-IDF esp_http_server ONLY (no AsyncTCP).
 *    · Handlers run on their own FreeRTOS task → blocking camera/SD work is safe
 *    · JPEG sent straight from the framebuffer → zero heap copy, no fragmentation
 *    · lru_purge_enable reaps sockets iOS Safari opens and abandons
 * ─────────────────────────────────────────────────────────────────
 */

#include "Arduino.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "SD_MMC.h"
#include "FS.h"
#include "WiFi.h"
#include "esp_wifi.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "video_writer.h"

// ═══════════════════════════════════════════════════════════════════════
//  PIN DEFINITIONS — OV3660
// ═══════════════════════════════════════════════════════════════════════
#define PWDN_GPIO_NUM    32
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM     0
#define SIOD_GPIO_NUM    26   // I2C SDA (SCCB)
#define SIOC_GPIO_NUM    27   // I2C SCL (SCCB)
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
#define LED_FLASH_GPIO    4   // GPIO4 = SD_MMC DAT1 in 4-bit mode → we use 1-bit

// ═══════════════════════════════════════════════════════════════════════
//  POWER PROFILE
//  LOW_POWER_MODE is a survival mode for running off a USB-serial adapter's
//  3V3 pin (~50 mA). It trades frame rate, resolution and range for roughly
//  a third of the current draw. It is NOT a substitute for a real 5 V supply
//  into the 5V pin — recording and high resolutions will still brown out.
//  Toggle it in platformio.ini via -DLOW_POWER_MODE.
// ═══════════════════════════════════════════════════════════════════════
#ifdef LOW_POWER_MODE
  #define CPU_MHZ       80                  // 80 MHz is the floor that still runs WiFi
  #define XCLK_HZ       10000000            // half clock ≈ half sensor current
  #define CAM_FB_COUNT  1                   // one framebuffer, less PSRAM churn
  #define DEFAULT_RES   FRAMESIZE_QVGA      // 320×240
  #define PHOTO_RES_MAX FRAMESIZE_SVGA      // QXGA capture spikes ~200 mA
  #define TX_POWER_Q    44                  // 11 dBm (units of 0.25 dBm)
  #define WIFI_PS_MODE  WIFI_PS_MIN_MODEM   // let the radio sleep between beacons
  #define AP_MAX_CLIENTS 1
  #define ENABLE_MJPEG  0                   // drop the port-81 server entirely
#else
  #define CPU_MHZ       240
  #define XCLK_HZ       20000000
  #define CAM_FB_COUNT  2
  #define DEFAULT_RES   FRAMESIZE_SVGA
  #define PHOTO_RES_MAX FRAMESIZE_QXGA
  #define TX_POWER_Q    68                  // 17 dBm
  #define WIFI_PS_MODE  WIFI_PS_NONE
  #define AP_MAX_CLIENTS 4
  #define ENABLE_MJPEG  1
#endif

// ═══════════════════════════════════════════════════════════════════════
//  WIFI AP
// ═══════════════════════════════════════════════════════════════════════
const char* AP_SSID = "wuw";
const char* AP_PASS = "wuwuwuwu";

// ═══════════════════════════════════════════════════════════════════════
//  GLOBAL STATE
// ═══════════════════════════════════════════════════════════════════════
static bool    flashOn     = false;
static bool    nightVision = false;
static bool    sdReady     = false;
static bool    camOk       = false;   // false → AP + UI still come up, camera disabled
static uint8_t streamRes   = DEFAULT_RES;
static uint8_t photoRes    = PHOTO_RES_MAX;
static int     photoCount  = 0;
static int     recFileNum  = 0;

static httpd_handle_t webHttpd    = NULL;
static httpd_handle_t streamHttpd = NULL;

// ═══════════════════════════════════════════════════════════════════════
//  CAMERA INIT
// ═══════════════════════════════════════════════════════════════════════
static bool initCamera() {
  // Zero-init: this struct has fields we never touch (sccb_i2c_port, …) and
  // stack garbage in them makes init fail intermittently between builds.
  camera_config_t cfg = {};
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
  cfg.xclk_freq_hz = XCLK_HZ;
  cfg.pixel_format = PIXFORMAT_JPEG;

  // CAMERA_GRAB_LATEST: always hand out the freshest frame and never stall
  // waiting for a consumer — essential when stream + recorder both pull frames.
  cfg.grab_mode    = CAMERA_GRAB_LATEST;
  cfg.fb_location  = CAMERA_FB_IN_PSRAM;

  if (psramFound()) {
    cfg.frame_size   = PHOTO_RES_MAX;    // alloc at max so any framesize fits later
    cfg.jpeg_quality = 12;
    cfg.fb_count     = CAM_FB_COUNT;
  } else {
    cfg.frame_size   = FRAMESIZE_SVGA;
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 1;
    cfg.fb_location  = CAMERA_FB_IN_DRAM;
    photoRes         = FRAMESIZE_UXGA;
    Serial.println("[CAM] No PSRAM — photo capped at UXGA");
  }

  // Retry with a hard sensor power-cycle between attempts. A 0x105 probe
  // failure is usually the sensor not answering on SCCB because its rail
  // hasn't come up yet (marginal supply) or the ribbon is seated poorly.
  esp_err_t err = ESP_FAIL;
  for (int attempt = 1; attempt <= 3; attempt++) {
    err = esp_camera_init(&cfg);
    if (err == ESP_OK) break;

    Serial.printf("[CAM] attempt %d/3 failed: 0x%x%s\n", attempt, err,
                  err == ESP_ERR_NOT_FOUND ? " (sensor did not answer on SCCB)" : "");
    esp_camera_deinit();

    // Toggle PWDN to force the sensor through a clean power-up
    pinMode(PWDN_GPIO_NUM, OUTPUT);
    digitalWrite(PWDN_GPIO_NUM, HIGH);   // powered down
    delay(200);
    digitalWrite(PWDN_GPIO_NUM, LOW);    // powered up
    delay(400);                          // let the rail settle before re-probing
  }
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init FAILED after 3 attempts: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, (framesize_t)streamRes);
  s->set_quality(s, 12);
  s->set_brightness(s, 1);
  s->set_contrast(s, 0);
  s->set_saturation(s, 0);
  s->set_sharpness(s, 0);
  s->set_special_effect(s, 0);
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);
  s->set_exposure_ctrl(s, 1);
  s->set_aec2(s, 1);
  s->set_aec_value(s, 300);
  s->set_gain_ctrl(s, 1);
  s->set_agc_gain(s, 0);
  s->set_gainceiling(s, GAINCEILING_2X);
  s->set_bpc(s, 1);
  s->set_wpc(s, 1);
  s->set_raw_gma(s, 1);
  s->set_lenc(s, 1);
  s->set_hmirror(s, 0);
  s->set_vflip(s, 1);
  s->set_dcw(s, 1);
  s->set_colorbar(s, 0);

  Serial.printf("[CAM] OK — PSRAM: %s\n", psramFound() ? "YES" : "NO");
  return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  SD CARD INIT  (1-bit MMC — keeps GPIO4 free for flash LED)
// ═══════════════════════════════════════════════════════════════════════
static bool initSD() {
  pinMode(2, INPUT_PULLUP);
  delay(10);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("[SD] Mount FAILED");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[SD] No card");
    return false;
  }
  Serial.printf("[SD] OK — %llu GB\n", SD_MMC.cardSize() / (1024ULL * 1024 * 1024));
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
    s->set_gainceiling(s, GAINCEILING_2X);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_brightness(s, 1);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
    s->set_lenc(s, 1);
    s->set_raw_gma(s, 1);
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
  }
}

// ═══════════════════════════════════════════════════════════════════════
//  PHOTO CAPTURE → SD
// ═══════════════════════════════════════════════════════════════════════
static String capturePhoto() {
  sensor_t* s = esp_camera_sensor_get();
  framesize_t saved = (framesize_t)streamRes;

  s->set_framesize(s, (framesize_t)photoRes);
  delay(350);                                   // sensor settle at new size
  camera_fb_t* fb = esp_camera_fb_get();        // discard stale frame
  if (fb) { esp_camera_fb_return(fb); fb = NULL; }
  delay(100);

  fb = esp_camera_fb_get();
  String result = "ERR_CAPTURE";
  if (fb) {
    if (sdReady) {
      String path = "/IMG_" + String(photoCount) + ".jpg";
      File f = SD_MMC.open(path.c_str(), FILE_WRITE);
      if (f) {
        f.write(fb->buf, fb->len);
        f.close();
        photoCount++;
        result = path + " " + String(fb->len / 1024) + "KB";
      } else result = "ERR_FILE_OPEN";
    } else result = "ERR_NO_SD";
    esp_camera_fb_return(fb);
  }

  s->set_framesize(s, saved);
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
<title>WUWCAM</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#000;color:#ccc;font:13px/1.5 monospace;height:100dvh;display:flex;flex-direction:column;overflow:hidden;-webkit-tap-highlight-color:transparent}
header{background:#0a0a0a;border-bottom:1px solid #1e1e1e;padding:6px 10px;display:flex;justify-content:space-between;align-items:center;flex-shrink:0;font-size:11px}
.logo{color:#0cf;font-size:14px;font-weight:bold;letter-spacing:.05em}
.main{flex:1;display:flex;overflow:hidden;min-height:0}
.sv{flex:1;background:#000;position:relative;display:flex;align-items:center;justify-content:center;overflow:hidden}
#si{max-width:100%;max-height:100%;display:block;object-fit:contain}
#se{display:none;position:absolute;color:#444;font-size:11px;text-align:center;padding:10px}
#se.on{display:block}
#rb{display:none;position:absolute;top:6px;right:6px;background:rgba(200,30,30,.9);color:#fff;font-size:11px;padding:2px 8px}
#rb.on{display:block}
#fps{position:absolute;bottom:4px;left:6px;color:#0cf;font-size:10px;opacity:.65}
.sb{width:210px;background:#060606;border-left:1px solid #1a1a1a;overflow-y:auto;padding:6px;flex-shrink:0;display:flex;flex-direction:column;gap:4px;-webkit-overflow-scrolling:touch}
.sb::-webkit-scrollbar{width:2px}
.sb::-webkit-scrollbar-thumb{background:#222}
h3{font-size:10px;color:#0cf;letter-spacing:.12em;border-bottom:1px solid #1a1a1a;padding-bottom:2px;margin-top:4px}
.row{display:flex;gap:4px}
button{background:#111;border:1px solid #2a2a2a;color:#999;font:12px monospace;padding:6px 4px;cursor:pointer;flex:1;touch-action:manipulation}
button:active{opacity:.6}
button.on{border-color:#0cf;color:#0cf;background:#001a22}
button.ron{border-color:#f44;color:#f44;background:#1a0000}
.lbl{font-size:10px;color:#555;margin-bottom:1px}
select{width:100%;background:#0d0d0d;color:#bbb;border:1px solid #222;font:11px monospace;padding:4px;-webkit-appearance:none;border-radius:0}
.sl{display:grid;grid-template-columns:52px 1fr 22px;gap:3px;align-items:center;font-size:10px;color:#555}
.sl input[type=range]{accent-color:#0cf;width:100%}
.ck{display:flex;flex-wrap:wrap;gap:8px}
.ck label{font-size:10px;color:#555;display:flex;align-items:center;gap:3px;cursor:pointer;touch-action:manipulation}
.ck input{accent-color:#0cf;width:14px;height:14px}
.cbar{padding:6px 8px;border-top:1px solid #1a1a1a;display:flex;gap:6px;flex-shrink:0}
.cbar button{padding:10px 8px;font-size:13px;flex:1}
#toast{position:fixed;bottom:56px;left:50%;transform:translateX(-50%);background:#0a0a0a;border:1px solid #0cf;color:#0cf;padding:4px 14px;font-size:11px;opacity:0;transition:opacity .2s;pointer-events:none;white-space:nowrap;z-index:99;max-width:90vw;overflow:hidden;text-overflow:ellipsis}
#toast.show{opacity:1}
#toast.err{border-color:#f44;color:#f44}
@media(max-width:820px){
  .main{flex-direction:column}
  .sv{height:52vw;flex:none;flex-shrink:0;min-height:150px;max-height:52vh}
  .sb{width:100%;border-left:none;border-top:1px solid #1a1a1a;flex:1;min-height:0}
}
</style>
</head>
<body>
<header>
  <span class="logo">WUWCAM</span>
  <span>SD:<span id="lsd">--</span> | <span id="lr">SVGA</span> | <span id="lp"></span></span>
</header>
<div class="main">
  <div class="sv">
    <img id="si" alt="" decoding="async">
    <div id="rb">&#9210; REC <span id="rt">00:00</span></div>
    <div id="fps">--</div>
    <div id="se">RECONNECTING...</div>
  </div>
  <div class="sb">

    <h3>CONTROLS</h3>
    <div class="row">
      <button id="btn-flash" onclick="toggleFlash()">&#9889; Flash</button>
      <button id="btn-night" onclick="toggleNight()">&#127769; Night</button>
    </div>
    <div class="row">
      <button id="btn-pause" onclick="togglePause()">&#10074;&#10074; Pause</button>
    </div>

    <h3>STREAM</h3>
    <div class="lbl">Resolution</div>
    <select id="sel-res" onchange="onRes(this)">
      <option value="4">QQVGA2 160x120</option>
      <option value="5">QVGA 320x240</option>
      <option value="6">CIF  400x296</option>
      <option value="8">VGA  640x480</option>
      <option value="9" selected>SVGA 800x600</option>
      <option value="10">XGA  1024x768</option>
      <option value="11">HD   1280x720</option>
      <option value="13">UXGA 1600x1200</option>
      <option value="14">FHD  1920x1080</option>
    </select>
    <div class="lbl">Stream speed</div>
    <select id="sel-rate" onchange="setRate(this.value)">
      <option value="0">Max (as fast as possible)</option>
      <option value="66" selected>~15 fps</option>
      <option value="100">~10 fps</option>
      <option value="200">~5 fps</option>
      <option value="500">~2 fps (weak signal)</option>
    </select>
    <div class="sl"><span>Quality</span>
      <input type="range" min="8" max="45" value="12"
        oninput="sv('vq',this.value)" onchange="api('/set?quality='+this.value)">
      <span id="vq">12</span>
    </div>

    <h3>IMAGE</h3>
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
    <select onchange="api('/set?wb='+this.value)">
      <option value="0">Auto</option>
      <option value="1">Sunny</option>
      <option value="2">Cloudy</option>
      <option value="3">Fluorescent</option>
      <option value="4">Incandescent</option>
    </select>
    <div class="sl"><span>Bright</span>
      <input type="range" min="-2" max="2" value="1"
        oninput="sv('vbr',this.value)" onchange="api('/set?bright='+this.value)">
      <span id="vbr">1</span>
    </div>
    <div class="sl"><span>Contrast</span>
      <input type="range" min="-2" max="2" value="0"
        oninput="sv('vco',this.value)" onchange="api('/set?contrast='+this.value)">
      <span id="vco">0</span>
    </div>
    <div class="sl"><span>Sat</span>
      <input type="range" min="-2" max="2" value="0"
        oninput="sv('vsa',this.value)" onchange="api('/set?sat='+this.value)">
      <span id="vsa">0</span>
    </div>
    <div class="sl"><span>Sharp</span>
      <input type="range" min="-2" max="2" value="0"
        oninput="sv('vsh',this.value)" onchange="api('/set?sharpness='+this.value)">
      <span id="vsh">0</span>
    </div>

    <h3>ORIENT</h3>
    <div class="ck">
      <label><input type="checkbox" onchange="api('/set?hmirror='+(this.checked?1:0))"> Mirror</label>
      <label><input type="checkbox" checked onchange="api('/set?vflip='+(this.checked?1:0))"> Flip</label>
    </div>

    <h3>RECORD CFG</h3>
    <div class="lbl">Record FPS</div>
    <select id="sel-rfps" onchange="recCfg()">
      <option value="5">5 fps</option>
      <option value="10" selected>10 fps</option>
      <option value="15">15 fps</option>
      <option value="20">20 fps</option>
      <option value="24">24 fps</option>
      <option value="30">30 fps (VGA/SVGA)</option>
      <option value="60">60 fps (QVGA only)</option>
    </select>
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

    <h3>EXPOSURE</h3>
    <div class="row">
      <button id="btn-aec" class="on" onclick="toggleAEC()">AEC AUTO</button>
      <button id="btn-agc" class="on" onclick="toggleAGC()">AGC AUTO</button>
    </div>
    <div class="sl"><span>AE Level</span>
      <input type="range" min="-2" max="2" value="0"
        oninput="sv('vael',this.value)" onchange="api('/set?ae_level='+this.value)">
      <span id="vael">0</span>
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
      <option value="0" selected>2x</option>
      <option value="1">4x</option>
      <option value="2">8x</option>
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

    <h3>INFO</h3>
    <div style="font-size:10px;color:#333;line-height:1.7">
      Photo: QXGA /IMG_####.jpg<br>
      Video: /VID_####.mp4<br>
      MJPEG: :81/stream (desktop)<br>
      AP: wuw / wuwuwuwu
    </div>
  </div>
</div>
<div class="cbar">
  <button id="btn-cap" onclick="doCapture()">&#128247; CAPTURE</button>
  <button id="btn-rec" onclick="toggleRec()">&#9210; REC</button>
</div>
<div id="toast"></div>
<script>
var img=document.getElementById('si'),se=document.getElementById('se');
var run=true,busy=false,gap=66,tmr=null,wd=null,nf=0,t0=Date.now();

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
  img.src='/jpg?'+(Date.now());
}
img.onload=function(){
  busy=false; clearTimeout(wd);
  se.classList.remove('on');
  if(++nf>=10){
    var f=(nf*1000/(Date.now()-t0)).toFixed(1);
    document.getElementById('fps').textContent=f+' fps';
    nf=0; t0=Date.now();
  }
  if(run) tmr=setTimeout(nextFrame,gap);
};
img.onerror=function(){
  busy=false; clearTimeout(wd);
  se.classList.add('on');
  if(run) tmr=setTimeout(nextFrame,1200);
};
function setRate(v){ gap=parseInt(v,10)||0; }
function togglePause(){
  run=!run;
  document.getElementById('btn-pause').classList.toggle('on',!run);
  if(run){ busy=false; nextFrame(); toast('Stream resumed'); }
  else { clearTimeout(tmr); clearTimeout(wd); toast('Stream paused'); }
}
/* Stop polling when the tab is backgrounded — iOS throttles hidden timers
   and would otherwise leave a half-finished request wedged on resume. */
document.addEventListener('visibilitychange',function(){
  if(document.hidden){ clearTimeout(tmr); clearTimeout(wd); busy=false; }
  else if(run){ busy=false; nextFrame(); }
});
nextFrame();

function api(u){return fetch(u).then(function(r){return r.json()}).catch(function(){return{}})}
function sv(id,v){document.getElementById(id).textContent=v}
var tT;
function toast(m,e){
  var t=document.getElementById('toast');
  t.textContent=m; t.className='show'+(e?' err':'');
  clearTimeout(tT); tT=setTimeout(function(){t.className=''},3000);
}

var flashOn=false,nightOn=false,recOn=false,aecAuto=true,agcAuto=true;
var recInt=null,recSecs=0;

function toggleFlash(){
  flashOn=!flashOn;
  api('/flash?v='+(flashOn?1:0)).then(function(d){
    flashOn=!!d.flash;
    document.getElementById('btn-flash').classList.toggle('on',flashOn);
    toast(flashOn?'Flash ON':'Flash OFF');
  });
}
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
  if(recOn){
    b.disabled=true; toast('Finalising MP4...');
    api('/rec/stop').then(function(d){
      b.disabled=false; recOn=false;
      clearInterval(recInt); recInt=null;
      rb.classList.remove('on');
      b.innerHTML='&#9210; REC'; b.classList.remove('ron');
      toast('Saved: '+(d.result||''));
    }).catch(function(){b.disabled=false;toast('Stop failed',true)});
  } else {
    api('/rec/start').then(function(d){
      if(!d.ok){toast(d.err||'Cannot start',true);return}
      recOn=true; recSecs=0;
      rb.classList.add('on');
      b.innerHTML='&#9209; STOP'; b.classList.add('ron');
      recInt=setInterval(function(){
        recSecs++;
        var m=String(Math.floor(recSecs/60)),s=String(recSecs%60);
        if(m.length<2)m='0'+m; if(s.length<2)s='0'+s;
        document.getElementById('rt').textContent=m+':'+s;
      },1000);
      toast('REC '+(d.file||''));
    }).catch(function(){toast('No response',true)});
  }
}
function doCapture(){
  toast('Capturing QXGA...');
  api('/capture').then(function(d){
    if(d.result&&d.result.indexOf('ERR')!==0){
      toast(d.result);
      document.getElementById('lp').textContent=d.count+' IMG';
    } else toast(d.result||'Fail',true);
  }).catch(function(){toast('No response',true)});
}
var RES={4:'QQVGA2',5:'QVGA',6:'CIF',8:'VGA',9:'SVGA',10:'XGA',11:'HD',12:'SXGA',13:'UXGA',14:'FHD'};
function onRes(sel){
  api('/set?res='+sel.value).then(function(){
    document.getElementById('lr').textContent=RES[sel.value]||sel.value;
    toast('Res: '+(RES[sel.value]||sel.value));
  });
}
function recCfg(){
  api('/rec/config?fps='+document.getElementById('sel-rfps').value+
      '&effect='+document.getElementById('sel-rfx').value+
      '&intensity='+document.getElementById('sl-ri').value+
      '&delay='+document.getElementById('sl-rd').value);
}
function pollStatus(){
  api('/status').then(function(d){
    if(!d||typeof d.sd==='undefined')return;
    document.getElementById('lsd').textContent=d.sd?'OK':'--';
    document.getElementById('lr').textContent=RES[d.res]||d.res;
    if(d.cam===false){
      run=false; clearTimeout(tmr); clearTimeout(wd);
      se.innerHTML='CAMERA NOT DETECTED (0x105)<br>'+
        'reseat the ribbon cable<br>and use a 5V supply';
      se.classList.add('on');
    }
    if(d.photos>0)document.getElementById('lp').textContent=d.photos+' IMG';
    if(!d.rec&&recOn){
      recOn=false; clearInterval(recInt); recInt=null;
      document.getElementById('rb').classList.remove('on');
      var b=document.getElementById('btn-rec');
      b.innerHTML='&#9210; REC'; b.classList.remove('ron');
      toast('Auto-stopped');
    }
  });
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
//  HANDLERS  — all run on the httpd task, so blocking here is safe
// ═══════════════════════════════════════════════════════════════════════

static esp_err_t h_index(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "identity");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// Single JPEG frame — sent straight from the framebuffer (no heap copy).
static esp_err_t h_jpg(httpd_req_t* req) {
  if (!camOk) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera offline");
    return ESP_FAIL;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no frame");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

static esp_err_t h_status(httpd_req_t* req) {
  String j = "{\"flash\":"  + String(flashOn     ? "true" : "false") +
             ",\"night\":"  + String(nightVision ? "true" : "false") +
             ",\"sd\":"     + String(sdReady     ? "true" : "false") +
             ",\"photos\":" + String(photoCount) +
             ",\"res\":"    + String(streamRes) +
             ",\"cam\":"    + String(camOk       ? "true" : "false") +
             ",\"heap\":"   + String(ESP.getFreeHeap()) +
             ",\"rec\":"    + String(rec_is_active() ? "true" : "false") + "}";
  return sendJson(req, j);
}

static esp_err_t h_capture(httpd_req_t* req) {
  if (!camOk)
    return sendJson(req, "{\"result\":\"ERR_NO_CAMERA\",\"count\":" + String(photoCount) + "}");
  if (rec_is_active())
    return sendJson(req, "{\"result\":\"ERR_RECORDING\",\"count\":" + String(photoCount) + "}");
  String r = capturePhoto();
  return sendJson(req, "{\"result\":\"" + r + "\",\"count\":" + String(photoCount) + "}");
}

static esp_err_t h_flash(httpd_req_t* req) {
  int v;
  if (qInt(req, "v", &v)) {
    flashOn = v != 0;
    digitalWrite(LED_FLASH_GPIO, flashOn ? HIGH : LOW);
  }
  return sendJson(req, "{\"flash\":" + String(flashOn ? "true" : "false") + "}");
}

static esp_err_t h_night(httpd_req_t* req) {
  int v;
  if (qInt(req, "v", &v)) {
    nightVision = v != 0;
    applyNightVision(nightVision);
  }
  return sendJson(req, "{\"night\":" + String(nightVision ? "true" : "false") + "}");
}

static esp_err_t h_set(httpd_req_t* req) {
  if (!camOk) return sendJson(req, "{\"ok\":false,\"err\":\"camera offline\"}");
  sensor_t* s = esp_camera_sensor_get();
  int v;
  if (qInt(req, "effect",      &v)) s->set_special_effect(s, v);
  if (qInt(req, "bright",      &v)) s->set_brightness(s, v);
  if (qInt(req, "contrast",    &v)) s->set_contrast(s, v);
  if (qInt(req, "sat",         &v)) s->set_saturation(s, v);
  if (qInt(req, "sharpness",   &v)) s->set_sharpness(s, v);
  if (qInt(req, "quality",     &v)) s->set_quality(s, v);
  if (qInt(req, "wb",          &v)) s->set_wb_mode(s, v);
  if (qInt(req, "hmirror",     &v)) s->set_hmirror(s, v);
  if (qInt(req, "vflip",       &v)) s->set_vflip(s, v);
  if (qInt(req, "aec",         &v)) s->set_exposure_ctrl(s, v);
  if (qInt(req, "agc",         &v)) s->set_gain_ctrl(s, v);
  if (qInt(req, "ae_level",    &v)) s->set_ae_level(s, v);
  if (qInt(req, "aec_val",     &v)) s->set_aec_value(s, v);
  if (qInt(req, "agc_gain",    &v)) s->set_agc_gain(s, v);
  if (qInt(req, "gainceiling", &v)) s->set_gainceiling(s, (gainceiling_t)v);
  if (qInt(req, "lenc",        &v)) s->set_lenc(s, v);
  if (qInt(req, "raw_gma",     &v)) s->set_raw_gma(s, v);
  if (qInt(req, "dcw",         &v)) s->set_dcw(s, v);
  if (qInt(req, "res", &v) && !rec_is_active()) {
    streamRes = (uint8_t)v;
    s->set_framesize(s, (framesize_t)streamRes);
    Serial.printf("[SET] Res → %d\n", streamRes);
  }
  return sendJson(req, "{\"ok\":true}");
}

static esp_err_t h_rec_start(httpd_req_t* req) {
  if (!camOk)           return sendJson(req, "{\"ok\":false,\"err\":\"Camera offline\"}");
  if (!sdReady)         return sendJson(req, "{\"ok\":false,\"err\":\"No SD card\"}");
  if (rec_is_active())  return sendJson(req, "{\"ok\":false,\"err\":\"Already recording\"}");
  String path = "/VID_" + String(recFileNum) + ".mp4";
  recFileNum++;
  bool ok = rec_start(path.c_str(), psramFound());
  return sendJson(req, ok ? "{\"ok\":true,\"file\":\"" + path + "\"}"
                          : "{\"ok\":false,\"err\":\"Start failed\"}");
}

static esp_err_t h_rec_stop(httpd_req_t* req) {
  return sendJson(req, "{\"ok\":true,\"result\":\"" + rec_stop() + "\"}");
}

static esp_err_t h_rec_config(httpd_req_t* req) {
  int fps = 10, fx = 0, inten = 5, dly = 3;
  qInt(req, "fps", &fps);
  qInt(req, "effect", &fx);
  qInt(req, "intensity", &inten);
  qInt(req, "delay", &dly);
  rec_set_config((uint8_t)fps, (uint8_t)fx, (uint8_t)inten, (uint8_t)dly);
  return sendJson(req, "{\"ok\":true}");
}

// ── MJPEG stream (port 81) — desktop convenience ─────────────────────────
#if ENABLE_MJPEG
#define BOUNDARY "wuwcam_frame"
static esp_err_t h_stream(httpd_req_t* req) {
  camera_fb_t* fb;
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
    fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    int n = snprintf(hdr, sizeof(hdr),
                     "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                     "Content-Length: %u\r\n\r\n", fb->len);

    esp_err_t r = httpd_resp_send_chunk(req, hdr, n);
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
    esp_camera_fb_return(fb);

    if (r != ESP_OK) break;      // client went away — release the socket
    vTaskDelay(pdMS_TO_TICKS(5)); // yield so the watchdog and WiFi task breathe
  }
  return ESP_OK;
}
#endif  // ENABLE_MJPEG

// ═══════════════════════════════════════════════════════════════════════
//  SERVER START
// ═══════════════════════════════════════════════════════════════════════
static void reg(httpd_handle_t h, const char* uri, esp_err_t (*fn)(httpd_req_t*)) {
  httpd_uri_t u = { .uri = uri, .method = HTTP_GET, .handler = fn, .user_ctx = NULL };
  httpd_register_uri_handler(h, &u);
}

static void startServers() {
  // ── Control + UI + snapshots (port 80) ──
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port      = 80;
  c.ctrl_port        = 32768;
  c.core_id          = 1;      // APP cpu — WiFi/LWIP own core 0
  c.task_priority    = 5;
  c.stack_size       = 12288;  // room for SD_MMC + FAT during /capture
  c.max_uri_handlers = 12;
  c.max_open_sockets = 7;      // LWIP ceiling on this build
  c.lru_purge_enable = true;   // reap the sockets iOS opens then abandons
  c.recv_wait_timeout = 5;
  c.send_wait_timeout = 5;

  if (httpd_start(&webHttpd, &c) == ESP_OK) {
    reg(webHttpd, "/",           h_index);
    reg(webHttpd, "/jpg",        h_jpg);
    reg(webHttpd, "/status",     h_status);
    reg(webHttpd, "/capture",    h_capture);
    reg(webHttpd, "/flash",      h_flash);
    reg(webHttpd, "/night",      h_night);
    reg(webHttpd, "/set",        h_set);
    reg(webHttpd, "/rec/start",  h_rec_start);
    reg(webHttpd, "/rec/stop",   h_rec_stop);
    reg(webHttpd, "/rec/config", h_rec_config);
    Serial.println("[WEB] :80 OK");
  } else {
    Serial.println("[WEB] :80 FAILED");
  }

#if ENABLE_MJPEG
  // ── MJPEG (port 81) — isolated so a hung stream can never clog port 80 ──
  httpd_config_t s = HTTPD_DEFAULT_CONFIG();
  s.server_port      = 81;
  s.ctrl_port        = 32769;
  s.core_id          = 1;
  s.task_priority    = 4;
  s.stack_size       = 6144;
  s.max_uri_handlers = 1;
  s.max_open_sockets = 2;
  s.lru_purge_enable = true;

  if (httpd_start(&streamHttpd, &s) == ESP_OK) {
    reg(streamHttpd, "/stream", h_stream);
    Serial.println("[WEB] :81/stream OK");
  } else {
    Serial.println("[WEB] :81 FAILED");
  }
#else
  Serial.println("[WEB] :81 MJPEG disabled (low power mode)");
#endif
}

// ═══════════════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════════════
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // camera inrush trips the detector

  // Serial FIRST, then the clock change: setCpuFrequencyMhz() reconfigures the
  // UART divisors of already-open ports to preserve the baud rate. Doing it in
  // the other order leaves the divisor sized for the old clock and the log
  // comes out as garbage at any baud.
  Serial.begin(115200);
  delay(200);
  setCpuFrequencyMhz(CPU_MHZ);
  delay(300);
  Serial.println("\n== WUW-CAM OV3660 ==");
#ifdef LOW_POWER_MODE
  Serial.println("[PWR] LOW POWER MODE — 80MHz / 10MHz XCLK / QVGA / 11dBm / no MJPEG");
  Serial.println("[PWR] for full performance use a 5V>=1A supply and drop -DLOW_POWER_MODE");
#endif

  pinMode(LED_FLASH_GPIO, OUTPUT);
  digitalWrite(LED_FLASH_GPIO, LOW);

  // Never hard-loop on camera failure: without the AP there is no way to see
  // what went wrong. Come up regardless and report the fault over the UI.
  camOk = initCamera();
  if (!camOk)
    Serial.println("[CAM] running WITHOUT camera — check ribbon seating & 5V supply");

  sdReady = initSD();

  // Let the rail recover before the radio starts pulling — on a marginal
  // supply, bringing WiFi up while the camera/SD inrush is still settling
  // is what collapses 3V3.
  delay(300);

  // ── WiFi AP ──
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, 1, 0, AP_MAX_CLIENTS);
  delay(300);
#if WIFI_PS_MODE == WIFI_PS_NONE
  WiFi.setSleep(false);            // no DTIM sleep → no 100 ms latency spikes
#else
  WiFi.setSleep(true);             // radio sleeps between beacons — big current saving
#endif
  esp_wifi_set_ps(WIFI_PS_MODE);
  esp_wifi_set_max_tx_power(TX_POWER_Q);

  IPAddress ip = WiFi.softAPIP();
  Serial.printf("[WiFi] AP %s | %s\n", AP_SSID, ip.toString().c_str());

  startServers();

  Serial.println("[READY]");
  Serial.printf("  UI:     http://%s\n", ip.toString().c_str());
  Serial.printf("  MJPEG:  http://%s:81/stream\n", ip.toString().c_str());
  Serial.printf("  SD:     %s\n", sdReady ? "OK" : "NOT FOUND");
  Serial.printf("  CPU:    %d MHz | Heap: %u | PSRAM: %u\n",
                getCpuFrequencyMhz(), ESP.getFreeHeap(), ESP.getFreePsram());
}

// ═══════════════════════════════════════════════════════════════════════
//  LOOP — everything is task-based; just report health
// ═══════════════════════════════════════════════════════════════════════
void loop() {
  static uint32_t last = 0;
  if (millis() - last > 30000) {
    last = millis();
    Serial.printf("[HEALTH] heap=%u psram=%u clients=%d rec=%d\n",
                  ESP.getFreeHeap(), ESP.getFreePsram(),
                  WiFi.softAPgetStationNum(), rec_is_active());
  }
  delay(1000);
}
