/* ── WUW CAM — RGB565-direct vs JPEG-decode, head to head ───────────────────
 *
 * Answers one question with measurements instead of estimates: for the LCD
 * viewfinder, how much do we gain by running the OV5640 in RGB565 and pushing
 * pixels straight to the panel, versus today's JPEG capture + software decode?
 *
 * Needs no display. The SPI stage is timed by clocking real DMA transfers out
 * of the panel pins with nothing attached — the bytes still take exactly as
 * long on the wire, so the number is honest.
 *
 *   pio run -e rgbbench -t upload && pio device monitor
 *
 * Stages timed per frame, identically in both phases:
 *   capture   esp_camera_fb_get()      — sensor + DVP + DMA
 *   decode    JPEG -> RGB565           — phase A only; phase B has none
 *   effect    one per-pixel pass       — stands in for the native effects
 *   push      320x240x16bpp over SPI   — real transfers, 40 MHz and 80 MHz
 */
#include <Arduino.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include <JPEGDEC.h>
#include "driver/spi_master.h"

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

/* Panel pins — same as lcdtest. Safe to drive with no panel attached. */
#define LCD_SCLK 47
#define LCD_MOSI 48
#define LCD_CS   21

#define PANEL_W 320
#define PANEL_H 240
#define FRAME_PX (PANEL_W * PANEL_H)
#define FRAME_BYTES (FRAME_PX * 2)

/* Pushed in tile-row chunks, the way a real driver streams to the panel. */
#define CHUNK_ROWS  16
#define CHUNK_BYTES (PANEL_W * CHUNK_ROWS * 2)
#define CHUNKS      (PANEL_H / CHUNK_ROWS)

#define WARMUP 5
#define FRAMES 40

static uint16_t* fbuf   = nullptr;   // working RGB565 frame (PSRAM)
static uint8_t*  dmaBuf = nullptr;   // DMA-capable chunk, internal SRAM
static JPEGDEC   jdec;
static spi_device_handle_t spiDev;
static bool      spiUp = false;

/* ── JPEGDEC writes straight into the panel-sized RGB565 frame ── */
static int jdDraw(JPEGDRAW* p) {
  for (int y = 0; y < p->iHeight; y++) {
    int dy = p->y + y;
    if (dy < 0 || dy >= PANEL_H) continue;
    uint16_t* src = p->pPixels + y * p->iWidth;
    uint16_t* dst = fbuf + dy * PANEL_W + p->x;
    int w = p->iWidth;
    if (p->x + w > PANEL_W) w = PANEL_W - p->x;
    if (w > 0) memcpy(dst, src, w * 2);
  }
  return 1;
}

/* ── the per-pixel effect both phases pay for (a cheap tint+contrast) ── */
static void effectPass(uint16_t* f) {
  for (int i = 0; i < FRAME_PX; i++) {
    uint16_t p = f[i];
    int r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    r = (r * 5) >> 2; if (r > 31) r = 31;      // warm the highlights
    b = (b * 3) >> 2;                          // cool the shadows
    g = (g * 9) >> 3; if (g > 63) g = 63;
    f[i] = (uint16_t)((r << 11) | (g << 5) | b);
  }
}

/* ── real SPI transfers; nothing is listening, the wire time is still real ── */
static bool spiBegin(int hz) {
  if (spiUp) { spi_bus_remove_device(spiDev); spi_bus_free(SPI2_HOST); spiUp = false; }
  spi_bus_config_t b = {};
  b.mosi_io_num = LCD_MOSI; b.miso_io_num = -1; b.sclk_io_num = LCD_SCLK;
  b.quadwp_io_num = -1; b.quadhd_io_num = -1;
  b.max_transfer_sz = CHUNK_BYTES + 64;
  if (spi_bus_initialize(SPI2_HOST, &b, SPI_DMA_CH_AUTO) != ESP_OK) return false;
  spi_device_interface_config_t d = {};
  d.clock_speed_hz = hz; d.mode = 0; d.spics_io_num = LCD_CS; d.queue_size = 4;
  if (spi_bus_add_device(SPI2_HOST, &d, &spiDev) != ESP_OK) { spi_bus_free(SPI2_HOST); return false; }
  spiUp = true; return true;
}
static void pushFrame(const uint16_t* f) {
  for (int c = 0; c < CHUNKS; c++) {
    memcpy(dmaBuf, (const uint8_t*)f + c * CHUNK_BYTES, CHUNK_BYTES);
    spi_transaction_t t = {};
    t.length = CHUNK_BYTES * 8;
    t.tx_buffer = dmaBuf;
    spi_device_transmit(spiDev, &t);
  }
}

int gFbCount = 2;
extern int gXclk;
static bool camInit(pixformat_t fmt, framesize_t size) {
  camera_config_t c = {};
  c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0;
  c.pin_d0=Y2_GPIO_NUM; c.pin_d1=Y3_GPIO_NUM; c.pin_d2=Y4_GPIO_NUM; c.pin_d3=Y5_GPIO_NUM;
  c.pin_d4=Y6_GPIO_NUM; c.pin_d5=Y7_GPIO_NUM; c.pin_d6=Y8_GPIO_NUM; c.pin_d7=Y9_GPIO_NUM;
  c.pin_xclk=XCLK_GPIO_NUM; c.pin_pclk=PCLK_GPIO_NUM;
  c.pin_vsync=VSYNC_GPIO_NUM; c.pin_href=HREF_GPIO_NUM;
  c.pin_sccb_sda=SIOD_GPIO_NUM; c.pin_sccb_scl=SIOC_GPIO_NUM;
  c.pin_pwdn=-1; c.pin_reset=-1;
  c.xclk_freq_hz=gXclk;
  c.pixel_format=fmt; c.frame_size=size;
  c.grab_mode=CAMERA_GRAB_LATEST; c.fb_location=CAMERA_FB_IN_PSRAM;
  c.jpeg_quality=12; c.fb_count=gFbCount;
  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) { Serial.printf("   camera init failed 0x%x\n", e); return false; }
  return true;
}

int gXclk = 20000000;      // swept by sensorSweep(); camInit() reads it

struct Result { double cap, dec, eff, push, total, fps; uint32_t kb; bool ok; };

/* ── How fast can the sensor actually go? ─────────────────────────────────
 * Every other number in this file measures a PIPELINE -- capture, decode,
 * effect, push -- which is the right thing to optimise but the wrong thing
 * to ask this question of. "Can we do 60 fps at 1080p" is a question about
 * the sensor and the DVP bus alone, so this measures those alone: grab a
 * frame, return it, grab another. No decode, no SPI, nothing downstream.
 *
 * XCLK is the one input nothing has ever varied. The driver has run at
 * 20 MHz since the first commit because that is what the example used. The
 * OV5640's pixel clock derives from it, so if there is headroom anywhere,
 * this is where it shows up -- and if there is not, that is worth knowing
 * before anyone designs around 60 fps.
 *
 * Sizes deliberately include HD and FHD even though they are expected to be
 * slow. An expected answer measured is worth more than an expected answer
 * assumed, and "1080p runs at N fps" is the number the question needs. */
struct SensorRate { double fps, msPerFrame; uint32_t kb; bool ok; };

static SensorRate rawRate(framesize_t size, int frames) {
  SensorRate r = {};
  if (!camInit(PIXFORMAT_JPEG, size)) return r;
  /* Discard generously: after a mode change the first frames come at the
     wrong exposure and, more importantly, at the wrong cadence. */
  for (int i = 0; i < 4; i++) { camera_fb_t* f = esp_camera_fb_get(); if (f) esp_camera_fb_return(f); }
  uint64_t bytes = 0;
  int got = 0;
  uint32_t t0 = micros();
  for (int i = 0; i < frames; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) break;
    bytes += fb->len; got++;
    esp_camera_fb_return(fb);
  }
  uint32_t dt = micros() - t0;
  esp_camera_deinit();
  if (!got || !dt) return r;
  r.fps = got * 1000000.0 / (double)dt;
  r.msPerFrame = dt / 1000.0 / got;
  r.kb = (uint32_t)(bytes / got / 1024);
  r.ok = true;
  return r;
}

static void sensorSweep() {
  const int   xclks[]  = { 10000000, 16000000, 20000000, 24000000, 30000000, 40000000 };
  struct Sz { framesize_t f; const char* n; int frames; };
  const Sz sizes[] = {
    { FRAMESIZE_QVGA, "QVGA  320x240",  20 },
    { FRAMESIZE_VGA,  "VGA   640x480",  16 },
    { FRAMESIZE_SVGA, "SVGA  800x600",  14 },
    { FRAMESIZE_HD,   "HD   1280x720",  10 },
    { FRAMESIZE_FHD,  "FHD  1920x1080",  8 },
  };
  Serial.println("\n=====================================================================");
  Serial.println("  SENSOR RATE — capture only. No decode, no SPI, no effect.");
  Serial.println("  This is the ceiling everything else lives under.");
  Serial.println("=====================================================================");
  Serial.print("  size            ");
  for (size_t k = 0; k < sizeof(xclks)/sizeof(xclks[0]); k++)
    Serial.printf("%6d ", xclks[k]/1000000);
  Serial.println(" MHz XCLK");
  Serial.println("  ---------------------------------------------------------------------");

  for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
    Serial.printf("  %-15s", sizes[i].n);
    uint32_t kbSeen = 0;
    for (size_t k = 0; k < sizeof(xclks)/sizeof(xclks[0]); k++) {
      gXclk = xclks[k];
      SensorRate r = rawRate(sizes[i].f, sizes[i].frames);
      if (r.ok) { Serial.printf("%6.1f ", r.fps); kbSeen = r.kb; }
      else      Serial.printf("%6s ", "--");
      delay(200);
    }
    Serial.printf("  fps   (%u KB/frame)\n", (unsigned)kbSeen);
  }
  gXclk = 20000000;
  Serial.println("  ---------------------------------------------------------------------");
  Serial.println("  \"--\" means the driver refused to initialise at that combination.");
  Serial.println("  Read the FHD row before designing anything around 1080p.");
}

static Result runJpeg(framesize_t size, int scale) {
  Result r = {};
  if (!camInit(PIXFORMAT_JPEG, size)) return r;
  uint64_t cap=0, dec=0, eff=0, push=0, bytes=0;
  int counted = 0;
  uint32_t wall0 = 0;
  for (int i = 0; i < WARMUP + FRAMES; i++) {
    uint32_t t0 = micros();
    camera_fb_t* fb = esp_camera_fb_get();
    uint32_t t1 = micros();
    if (!fb) continue;
    /* VGA -> 320x240 is exactly JPEG_SCALE_HALF, so the decoder does the
       downscale for free instead of us resampling afterwards. */
    if (jdec.openRAM(fb->buf, fb->len, jdDraw)) {
      jdec.setPixelType(RGB565_LITTLE_ENDIAN);
      jdec.decode(0, 0, scale);
      jdec.close();
    }
    uint32_t t2 = micros();
    effectPass(fbuf);
    uint32_t t3 = micros();
    pushFrame(fbuf);
    uint32_t t4 = micros();
    if (i >= WARMUP) {
      if (!counted) wall0 = t0;
      cap += t1-t0; dec += t2-t1; eff += t3-t2; push += t4-t3;
      bytes += fb->len; counted++;
    }
    esp_camera_fb_return(fb);
    if (i == WARMUP + FRAMES - 1 && counted)
      r.fps = counted * 1000000.0 / (double)(t4 - wall0);
  }
  esp_camera_deinit();
  if (!counted) return r;
  r.cap=cap/1000.0/counted; r.dec=dec/1000.0/counted;
  r.eff=eff/1000.0/counted; r.push=push/1000.0/counted;
  r.total=r.cap+r.dec+r.eff+r.push; r.kb=bytes/counted/1024; r.ok=true;
  return r;
}

static Result runRgb(int fbCount) {
  Result r = {};
  extern int gFbCount; gFbCount = fbCount;
  if (!camInit(PIXFORMAT_RGB565, FRAMESIZE_QVGA)) return r;
  gFbCount = 2;
  uint64_t cap=0, dec=0, eff=0, push=0;
  int counted = 0; uint32_t wall0 = 0;
  for (int i = 0; i < WARMUP + FRAMES; i++) {
    uint32_t t0 = micros();
    camera_fb_t* fb = esp_camera_fb_get();
    uint32_t t1 = micros();
    if (!fb) continue;
    /* No decode at all — the sensor already handed us panel-format pixels.
       One copy into our working buffer is all that stands between the DVP
       DMA landing zone and the effect pass. */
    size_t n = fb->len < FRAME_BYTES ? fb->len : FRAME_BYTES;
    memcpy(fbuf, fb->buf, n);
    esp_camera_fb_return(fb);
    uint32_t t2 = micros();
    effectPass(fbuf);
    uint32_t t3 = micros();
    pushFrame(fbuf);
    uint32_t t4 = micros();
    if (i >= WARMUP) {
      if (!counted) wall0 = t0;
      /* Capture and copy accounted SEPARATELY. They were added together as
         "cap", which made the sensor look responsible for a 150 KB PSRAM-to-
         PSRAM memcpy it has nothing to do with. The conclusion that this path
         is slower still holds -- fb_get alone dominates -- but the column now
         says which part is the sensor and which part is us. The copy lands in
         the "dec" column, which is otherwise zero here. */
      cap += t1-t0; dec += t2-t1; eff += t3-t2; push += t4-t3; counted++;
    }
    if (i == WARMUP + FRAMES - 1 && counted)
      r.fps = counted * 1000000.0 / (double)(t4 - wall0);
  }
  esp_camera_deinit();
  if (!counted) return r;
  r.cap=cap/1000.0/counted; r.dec=dec/1000.0/counted;   // dec = the copy
  r.eff=eff/1000.0/counted; r.push=push/1000.0/counted;
  r.total=r.cap+r.dec+r.eff+r.push; r.kb=FRAME_BYTES/1024; r.ok=true;
  return r;
}

static void report(const char* name, const Result& r) {
  if (!r.ok) { Serial.printf("  %-22s  FAILED\n", name); return; }
  Serial.printf("  %-22s %7.1f %7.1f %7.1f %7.1f %8.1f %7.1f  %4u\n",
                name, r.cap, r.dec, r.eff, r.push, r.total, r.fps, r.kb);
}

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n== WUW CAM — RGB565-direct vs JPEG-decode ==");
  setCpuFrequencyMhz(240);
  Serial.printf("[CPU] %d MHz\n", getCpuFrequencyMhz());

  fbuf   = (uint16_t*)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM);
  dmaBuf = (uint8_t*) heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  if (!fbuf || !dmaBuf) {
    Serial.printf("[MEM] alloc failed (frame %s, dma %s)\n",
                  fbuf?"ok":"FAILED", dmaBuf?"ok":"FAILED");
    return;
  }
  memset(fbuf, 0, FRAME_BYTES);
  Serial.printf("[MEM] frame %u B in PSRAM, DMA chunk %u B internal\n",
                (unsigned)FRAME_BYTES, (unsigned)CHUNK_BYTES);

  const int rates[2] = {40000000, 80000000};
  for (int k = 0; k < 2; k++) {
    Serial.printf("\n[SPI] %d MHz — %d chunks of %u B per frame\n",
                  rates[k]/1000000, CHUNKS, (unsigned)CHUNK_BYTES);
    if (!spiBegin(rates[k])) { Serial.println("  SPI init failed"); continue; }
    Serial.println("  path                    cap    dec    eff   push    total     fps    KB");
    Serial.println("  ----------------------------------------------------------------------");
    Result a = runJpeg(FRAMESIZE_VGA,  JPEG_SCALE_HALF);  report("A  VGA jpeg, 1/2 dec", a);
    delay(300);
    Result c = runJpeg(FRAMESIZE_QVGA, 0 /* 1:1 */);  report("C  QVGA jpeg, 1:1", c);
    delay(300);
    Result b = runRgb(2);                                 report("B  RGB565 fb=2", b);
    delay(300);
    Result d = runRgb(3);                                 report("D  RGB565 fb=3", d);
    Serial.printf("  ----------------------------------------------------------------------\n");
    if (a.ok && c.ok)
      Serial.printf("  C vs A: decode %.1f -> %.1f ms   frame %.1f -> %.1f ms   fps %.1f -> %.1f  (%.2fx)\n",
                    a.dec, c.dec, a.total, c.total, a.fps, c.fps, a.fps>0? c.fps/a.fps : 0.0);
    if (a.ok && b.ok)
      Serial.printf("  B vs A: fps %.1f -> %.1f  (%.2fx)\n", a.fps, b.fps, a.fps>0? b.fps/a.fps : 0.0);
    delay(300);
  }
  sensorSweep();
  Serial.println("\n[DONE] numbers above are measured, not modelled.");
}

void loop() { delay(5000); }
