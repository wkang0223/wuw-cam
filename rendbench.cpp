/*
 * rendbench.cpp — camera→pixels benchmark, no display required
 *
 * The panel is only the last 30 ms of the pipeline. Everything expensive
 * happens before it, so all of it can be measured with nothing wired.
 *
 * Measures, on the SAME frame so the comparison is fair:
 *   · decode at 1:1, 1/2, 1/4        — what the scale factor actually buys
 *   · block-streaming into RGB565    — the path a custom renderer would use
 *   · inter-frame tile change rate   — whether the neuromorphic idea pays off,
 *                                      measured in your real room rather than
 *                                      assumed
 *
 * Then it projects SPI time from the measured byte counts, so we can see the
 * whole budget before committing to writing a renderer.
 *
 *   pio run -e rendbench -t upload
 */

#include "Arduino.h"
#include "esp_camera.h"
#include "esp_jpg_decode.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <JPEGDEC.h>

#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM    4
#define SIOC_GPIO_NUM    5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM      8
#define Y3_GPIO_NUM      9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM   13

#define PANEL_W 320
#define PANEL_H 240
#define TILE    16
#define TILES_X (PANEL_W / TILE)
#define TILES_Y (PANEL_H / TILE)
#define SPI_HZ  40000000.0        // 40 MHz = 5 MB/s on the wire

static uint8_t* rgb888  = nullptr;   // decode target for the timed runs
static uint16_t* cur565 = nullptr;   // active destination
static uint16_t* psr565 = nullptr;   // panel-sized, in PSRAM
static uint16_t* int565 = nullptr;   // panel-sized, in internal SRAM
#define INTJPG_MAX 40960
static uint8_t*  intJpg = nullptr;   // compressed source, internal SRAM
static uint16_t* ret565 = nullptr;   // "retina": what a panel would already show
static size_t   jpgLen = 0;
static uint8_t* jpgBuf = nullptr;
static uint16_t dstW = 0, dstH = 0;

/* Second decoder under test. tjpgd's floor measured 171 ms with no writer at
   all, so the decoder itself — not memory, not scaling, not our code — is the
   entire bottleneck. JPEGDEC emits RGB565 directly (no RGB888 middleman) and
   has its own scaling, so this is a like-for-like swap of the slow part. */
static JPEGDEC jdec;
static int jdDraw(JPEGDRAW* p) {
  for (int r = 0; r < p->iHeight; r++) {
    int py = p->y + r;
    if (py >= PANEL_H) break;
    int n = (p->x + p->iWidth > PANEL_W) ? (PANEL_W - p->x) : p->iWidth;
    if (n > 0) memcpy(cur565 + (size_t)py*PANEL_W + p->x,
                      p->pPixels + (size_t)r*p->iWidth, (size_t)n*2);
  }
  return 1;
}

// RGB565 -> approximate luma, for honest frame differencing
static inline int luma565(uint16_t p) {
  int r = ((p >> 11) & 0x1F) << 3, g = ((p >> 5) & 0x3F) << 2, b = (p & 0x1F) << 3;
  return (77 * r + 150 * g + 29 * b) >> 8;
}

static size_t reader(void* a, size_t i, uint8_t* buf, size_t len) {
  // A NULL buffer is a SEEK request, not a read — the decoder still
  // expects the byte count back. Returning 0 here reads as a failure.
  if (i + len > jpgLen) len = jpgLen - i;
  if (!buf) return len;
  memcpy(buf, jpgBuf + i, len);
  return len;
}

// does nothing — measures the decoder alone, with zero writer cost
static bool wrNull(void* a, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* d) {
  return true;
}

// plain assembly into RGB888 — used to time raw decode at each scale
static bool wrRGB(void* a, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* d) {
  if (!d) return true;
  for (uint16_t r = 0; r < h; r++) {
    size_t off = (((size_t)(y + r) * dstW) + x) * 3;
    if (off + (size_t)w * 3 > (size_t)dstW * dstH * 3) continue;
    memcpy(rgb888 + off, d + (size_t)r * w * 3, (size_t)w * 3);
  }
  return true;
}

// what a real renderer does: convert each block to RGB565 as it emerges,
// straight into the panel-sized buffer. No full-size intermediate.
static bool wr565(void* a, uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t* d) {
  if (!d) return true;
  for (uint16_t r = 0; r < h; r++) {
    uint16_t py = y + r;
    if (py >= PANEL_H) break;
    uint8_t*  s   = d + (size_t)r * w * 3;
    uint16_t* dst = cur565 + (size_t)py * PANEL_W + x;
    uint16_t  n   = (x + w > PANEL_W) ? (PANEL_W - x) : w;
    for (uint16_t i = 0; i < n; i++)
      dst[i] = ((s[i*3] & 0xF8) << 8) | ((s[i*3+1] & 0xFC) << 3) | (s[i*3+2] >> 3);
  }
  return true;
}

static bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0;
  c.pin_d0=Y2_GPIO_NUM; c.pin_d1=Y3_GPIO_NUM; c.pin_d2=Y4_GPIO_NUM; c.pin_d3=Y5_GPIO_NUM;
  c.pin_d4=Y6_GPIO_NUM; c.pin_d5=Y7_GPIO_NUM; c.pin_d6=Y8_GPIO_NUM; c.pin_d7=Y9_GPIO_NUM;
  c.pin_xclk=XCLK_GPIO_NUM; c.pin_pclk=PCLK_GPIO_NUM;
  c.pin_vsync=VSYNC_GPIO_NUM; c.pin_href=HREF_GPIO_NUM;
  c.pin_sccb_sda=SIOD_GPIO_NUM; c.pin_sccb_scl=SIOC_GPIO_NUM;
  c.pin_pwdn=-1; c.pin_reset=-1;
  c.xclk_freq_hz=20000000; c.pixel_format=PIXFORMAT_JPEG;
  c.grab_mode=CAMERA_GRAB_LATEST; c.fb_location=CAMERA_FB_IN_PSRAM;
  c.frame_size=FRAMESIZE_VGA;   // 1/2 of VGA is exactly the panel size
  c.jpeg_quality=12; c.fb_count=2;
  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) { Serial.printf("[CAM] init failed 0x%x\n", e); return false; }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(700);
  Serial.println("\n== WUW CAM — render pipeline benchmark (no display needed) ==");
  Serial.printf("[CPU] %d MHz  (the main firmware forces 240)\n", getCpuFrequencyMhz());
  setCpuFrequencyMhz(240);
  Serial.printf("[CPU] now %d MHz\n", getCpuFrequencyMhz());

  if (!initCamera()) return;
  sensor_t* s = esp_camera_sensor_get();
  Serial.printf("[CAM] OK  sensor 0x%02x\n", s ? s->id.PID : 0);

  rgb888 = (uint8_t*)heap_caps_malloc(640UL*480*3, MALLOC_CAP_SPIRAM);
  psr565 = (uint16_t*)heap_caps_malloc(PANEL_W*PANEL_H*2, MALLOC_CAP_SPIRAM);
  cur565 = psr565;
  int565 = (uint16_t*)heap_caps_malloc(PANEL_W*PANEL_H*2, MALLOC_CAP_INTERNAL);
  intJpg = (uint8_t*)heap_caps_malloc(INTJPG_MAX, MALLOC_CAP_INTERNAL);
  ret565 = (uint16_t*)heap_caps_malloc(PANEL_W*PANEL_H*2, MALLOC_CAP_SPIRAM);
  if (!rgb888 || !psr565 || !ret565) { Serial.println("[MEM] PSRAM alloc failed"); return; }
  Serial.printf("[MEM] internal RGB565 %s | internal jpeg buf %s | DRAM free %u\n",
                int565 ? "OK" : "FAILED", intJpg ? "OK" : "FAILED",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  memset(ret565, 0, PANEL_W*PANEL_H*2);
  Serial.printf("[MEM] buffers ok, PSRAM free %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println("[BENCH] warming up...\n");
}

void loop() {
  static uint32_t n=0, jpgKB=0;
  static uint64_t aNone=0, a2X=0, a4X=0, aStream=0, aInt=0, aBoth=0, aNull=0, aJd=0;
  static uint32_t changedAcc=0, firstFrame=1;
  if (!rgb888) { delay(1000); return; }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { delay(5); return; }
  jpgBuf = fb->buf; jpgLen = fb->len;
  jpgKB += fb->len / 1024;

  uint64_t t;

  // ── decoder with no writer at all: the floor ──
  t = esp_timer_get_time();
  esp_jpg_decode(jpgLen, JPG_SCALE_NONE, reader, wrNull, nullptr);
  aNull += esp_timer_get_time() - t;

  // ── raw decode cost at each scale, same frame each time ──
  dstW = fb->width;      dstH = fb->height;
  t = esp_timer_get_time();
  esp_jpg_decode(jpgLen, JPG_SCALE_NONE, reader, wrRGB, nullptr);
  aNone += esp_timer_get_time() - t;

  dstW = fb->width/2;    dstH = fb->height/2;
  t = esp_timer_get_time();
  esp_jpg_decode(jpgLen, JPG_SCALE_2X, reader, wrRGB, nullptr);
  a2X += esp_timer_get_time() - t;

  dstW = fb->width/4;    dstH = fb->height/4;
  t = esp_timer_get_time();
  esp_jpg_decode(jpgLen, JPG_SCALE_4X, reader, wrRGB, nullptr);
  a4X += esp_timer_get_time() - t;

  // ── stream into PSRAM (what we measured before) ──
  cur565 = psr565;
  t = esp_timer_get_time();
  esp_jpg_decode(jpgLen, JPG_SCALE_2X, reader, wr565, nullptr);
  aStream += esp_timer_get_time() - t;

  // ── same decode, destination in INTERNAL SRAM ──
  if (int565) {
    cur565 = int565;
    t = esp_timer_get_time();
    esp_jpg_decode(jpgLen, JPG_SCALE_2X, reader, wr565, nullptr);
    aInt += esp_timer_get_time() - t;
  }

  // ── and with the compressed source copied to internal SRAM too ──
  if (int565 && intJpg && jpgLen <= INTJPG_MAX) {
    memcpy(intJpg, jpgBuf, jpgLen);
    uint8_t* save = jpgBuf; jpgBuf = intJpg;
    cur565 = int565;
    t = esp_timer_get_time();
    esp_jpg_decode(jpgLen, JPG_SCALE_2X, reader, wr565, nullptr);
    aBoth += esp_timer_get_time() - t;
    jpgBuf = save;
  }
  cur565 = psr565;

  // ── JPEGDEC, half scale, straight to RGB565 ──
  t = esp_timer_get_time();
  if (jdec.openRAM(jpgBuf, jpgLen, jdDraw)) {
    jdec.setPixelType(RGB565_LITTLE_ENDIAN);
    jdec.decode(0, 0, JPEG_SCALE_HALF);
    jdec.close();
  }
  aJd += esp_timer_get_time() - t;

  esp_camera_fb_return(fb);

  // ── how much of the frame actually changed? (the neuromorphic question) ──
  uint32_t changed = 0;
  for (int ty = 0; ty < TILES_Y; ty++) {
    for (int tx = 0; tx < TILES_X; tx++) {
      uint32_t diff = 0, samples = 0;
      for (int r = 0; r < TILE; r += 4) {
        uint16_t* a = cur565 + (size_t)(ty*TILE+r)*PANEL_W + tx*TILE;
        uint16_t* b = ret565 + (size_t)(ty*TILE+r)*PANEL_W + tx*TILE;
        for (int i = 0; i < TILE; i += 2) {
          int d = luma565(a[i]) - luma565(b[i]);       // compare brightness,
          diff += (d < 0 ? -d : d);                    // not packed bit values
          samples++;
        }
      }
      if (samples && (diff / samples) > 8) changed++;  // >8/255 mean = visible
    }
  }
  if (firstFrame) { firstFrame = 0; changed = TILES_X*TILES_Y; }
  changedAcc += changed;
  memcpy(ret565, cur565, PANEL_W*PANEL_H*2);

  if (++n % 10 == 0) {
    float mNone=aNone/10000.0f, m2X=a2X/10000.0f, m4X=a4X/10000.0f, mSt=aStream/10000.0f;
    float chg = changedAcc / 10.0f;
    float pct = 100.0f * chg / (TILES_X*TILES_Y);
    float fullKB = PANEL_W*PANEL_H*2 / 1024.0f;
    float chgKB  = chg * TILE*TILE*2 / 1024.0f;
    float spiFull = (PANEL_W*PANEL_H*2) / SPI_HZ * 8 * 1000.0f;   // ms
    float spiChg  = (chg * TILE*TILE*2) / SPI_HZ * 8 * 1000.0f;

    Serial.printf("\n[BENCH] %u frames · jpeg avg %u KB\n", n, jpgKB/n);
    Serial.printf("  decode ONLY (null writer) %6.1f ms   <- decoder floor\n", aNull/10000.0f);
    Serial.printf("  decode 1:1  %ux%u   %6.1f ms   (writer costs %.1f ms)\n",
                  640,480, mNone, mNone - aNull/10000.0f);
    Serial.printf("  decode 1/2  %ux%u   %6.1f ms   (%.2fx cheaper)\n",320,240,m2X,mNone/m2X);
    Serial.printf("  decode 1/4  %ux%u   %6.1f ms   (%.2fx cheaper)\n",160,120,m4X,mNone/m4X);
    Serial.printf("  stream 1/2 -> PSRAM       %6.1f ms\n", mSt);
    Serial.printf("  stream 1/2 -> INTERNAL    %6.1f ms   (%.2fx)\n",
                  aInt/10000.0f, aInt ? mSt/(aInt/10000.0f) : 0.0f);
    Serial.printf("  stream, src+dst INTERNAL  %6.1f ms   (%.2fx)  <- best case\n",
                  aBoth/10000.0f, aBoth ? mSt/(aBoth/10000.0f) : 0.0f);
    Serial.printf("  JPEGDEC 1/2 -> RGB565     %6.1f ms   (%.2fx vs tjpgd)  <- alt decoder\n",
                  aJd/10000.0f, aJd ? mSt/(aJd/10000.0f) : 0.0f);
    Serial.printf("  tiles changed  %.0f/%d (%.0f%%)  %.0f KB vs %.0f KB full\n",
                  chg, TILES_X*TILES_Y, pct, chgKB, fullKB);
    Serial.printf("  SPI @40MHz: full %.1f ms | changed-only %.1f ms\n", spiFull, spiChg);
    Serial.printf("  => naive  %.1f ms (%.1f fps)   [1:1 decode + full push]\n",
                  mNone+spiFull, 1000.0f/(mNone+spiFull));
    Serial.printf("  => scaled %.1f ms (%.1f fps)   [1/2 stream + full push]\n",
                  mSt+spiFull, 1000.0f/(mSt+spiFull));
    Serial.printf("  => +change %.1f ms (%.1f fps)  [1/2 stream + changed tiles]\n",
                  mSt+spiChg, 1000.0f/(mSt+spiChg));
    aNone=a2X=a4X=aStream=aInt=aBoth=aNull=aJd=0; changedAcc=0; jpgKB=0; n=0;
  }
}
