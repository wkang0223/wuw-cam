/*
 * lcdtest.cpp — ST7789 display/render benchmark.
 *
 * Touch is intentionally not attached here. This HR2046 clone needs one
 * conversion per CS window, which LovyanGFX does not provide. Use touch_diag
 * for hardware diagnosis and panel.cpp for the production reader.
 *
 * rendbench settled three things on real hardware, and two of them were the
 * opposite of what I predicted:
 *
 *   · tjpgd decode was 224 ms; JPEGDEC does the same work in 60 ms (3.7x).
 *     The decoder was the entire bottleneck — not SPI, not PSRAM, not scaling.
 *   · JPG_SCALE did NOT reduce tjpgd's work (222 ms vs 197 ms — slower).
 *   · Memory placement was irrelevant: source and destination in internal SRAM
 *     measured 223 ms against PSRAM's 224 ms.
 *
 * And once the frame-difference metric was fixed (comparing luma instead of
 * packed RGB565 bits) only 6-18% of tiles change between frames, so pushing
 * just the changed ones cuts transfer roughly 17x.
 *
 * So this build does the two things worth doing and measures them against the
 * naive path, with the panel really contending for the bus:
 *   JPEGDEC at half scale  ->  RGB565  ->  push only changed tiles
 *
 *   pio run -e lcdtest -t upload
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <JPEGDEC.h>
#include "Arduino.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "SD_MMC.h"

// ── camera pins (identical to the working firmware) ──
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

// ── display pins ──
// Must stay identical to panel.cpp: this bench and the shipping firmware run
// on the same wiring, and a bench that passes on a different pin map is worse
// than no bench. See panel.cpp for the production pin map.
#define LCD_MISO  1     // module SDO + T_DO bridged   (B3)
#define LCD_MOSI  2     // module SDI + T_DIN bridged  (B2)
#define LCD_SCLK 41     // module SCK + T_CLK bridged  (B1)
#define TCH_CS   42
#define LCD_BL   48
#define LCD_DC   47
#define LCD_CS   21
#define LCD_RST  20
#define LCD_SPI_HZ 40000000    // try 80000000 once the panel is wired short

#define PW   320
#define PH   240
#define TILE 16
#define TX   (PW / TILE)
#define TY   (PH / TILE)

/* ── Pocket-camera layout, 320x240, operated by thumbs ─────────────────
   status strip · viewfinder · one row of 64 px keys. 64x60 is comfortably
   above the 44 px minimum, and five keys is all a camera needs in reach. */
#define ST_H   20                 // status strip
#define VF_Y   ST_H
#define VF_H   160                // viewfinder window
#define BAR_Y  (VF_Y + VF_H)
#define BAR_H  (PH - BAR_Y)
#define NBTN   5
#define BTN_W  (PW / NBTN)

/* The browser's 33 shaders are WebGL and cannot run here. These are the
   ones that are just arithmetic per pixel — a few ms on 320x240 — so the
   panel gets its own set rather than none. */
enum { FX_NONE=0, FX_MONO, FX_INVERT, FX_THERMAL, FX_POSTER, FX_EDGE, FX_COUNT };
static const char* FX_NAME[FX_COUNT] = { "RAW","MONO","NEG","THERM","POSTER","EDGE" };
static uint8_t uiFx = FX_NONE;
static bool    uiRec = false;

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel;
  lgfx::Bus_SPI       _bus;
public:
  LGFX() {
    { auto c = _bus.config();
      c.spi_host=SPI2_HOST; c.spi_mode=0;
      /* 40 MHz pushes a full frame in 34 ms, 80 MHz in 19 ms (measured).
         80 needs short, tidy wiring — on long dupont leads the panel shows
         torn or speckled tiles. Start at 40; raise once the wiring is neat. */
      c.freq_write=LCD_SPI_HZ; c.freq_read=6000000;
      c.spi_3wire=false; c.use_lock=true; c.dma_channel=SPI_DMA_CH_AUTO;
      c.pin_sclk=LCD_SCLK; c.pin_mosi=LCD_MOSI; c.pin_miso=LCD_MISO; c.pin_dc=LCD_DC;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs=LCD_CS; c.pin_rst=LCD_RST; c.pin_busy=-1;
      c.panel_width=240; c.panel_height=320;
      c.bus_shared=true; c.readable=false; c.invert=false; c.rgb_order=false;
      _panel.config(c); }
    setPanel(&_panel);
  }
};

static LGFX      lcd;
static JPEGDEC   jdec;
static bool      lcdOK=false, camOK=false;
static uint16_t* cur=nullptr;      // freshly decoded frame
static uint16_t* ret=nullptr;      // "retina": what the glass already shows
static uint16_t* run=nullptr;      // staging for one coalesced tile run
static uint8_t   dirty[TY][TX];

/* ── capture runs on core 0 so the sensor wait overlaps the render ──────────
 * esp_camera_fb_get() blocks until the sensor delivers. Done inline that wait
 * is dead time; done on the other core it costs nothing, because core 1 is
 * pushing the previous frame while core 0 waits for the next one.
 * MODE_RGB565 additionally skips JPEG decode entirely — the OV5640 hands us
 * panel-format pixels, so there is nothing to decode.  */
#define MODE_JPEG   0
#define MODE_RGB565 1
static volatile int  camMode  = MODE_JPEG;
static volatile int  wantMode = MODE_JPEG;     // set by the MENU key
static uint16_t*     capBuf[2] = {nullptr,nullptr};
static volatile int  capReady = -1;            // index holding a complete frame
static volatile uint32_t capSeq = 0, capUs = 0, capDecUs = 0;
static SemaphoreHandle_t capMux = nullptr;
static uint16_t*     jdTarget = nullptr;       // where jdDraw writes

static inline int luma565(uint16_t p) {
  int r=((p>>11)&0x1F)<<3, g=((p>>5)&0x3F)<<2, b=(p&0x1F)<<3;
  return (77*r + 150*g + 29*b) >> 8;
}
  // timing probe, writes nothing

static int jdDraw(JPEGDRAW* p) {
  uint16_t* dst = jdTarget;
  if (!dst) return 0;
  for (int r = 0; r < p->iHeight; r++) {
    int py = p->y + r;
    if (py >= PH) break;
    int n = (p->x + p->iWidth > PW) ? (PW - p->x) : p->iWidth;
    if (n > 0) memcpy(dst + (size_t)py*PW + p->x,
                      p->pPixels + (size_t)r*p->iWidth, (size_t)n*2);
  }
  return 1;
}

/* ── WUW: the camera's own visual language ────────────────────────────────
 * The same surface model as the web UI, rebuilt for a 320x240 RGB565 panel.
 * Four rules, applied to every panel the device draws:
 *
 *   horizon   a vertical ramp whose direction FLIPS at 47% height, so the
 *             surface reads as a curved body catching light rather than a
 *             flat fill. The flip line is the brightest row -- the waterline.
 *   rim       one pixel of light on the top and left edge only.
 *   bevel     a light top lip and a dark bottom, inverted while pressed so a
 *             key physically sinks.
 *   shadow    a hard contact line directly beneath, plus a softer ambient
 *             band under that. Two shadows, not one -- it is what separates
 *             an object sitting on a surface from a sticker.
 *
 * RGB565 gives 5/6/5 bits, so a smooth ramp bands visibly. Every gradient row
 * is ordered-dithered before quantising, which costs nothing and removes it.
 */
typedef struct {
  const char* name;
  uint8_t bg[3];        // deep ground behind everything
  uint8_t hiTop[3];     // panel, top of the upper ramp
  uint8_t horizon[3];   // the waterline -- brightest point
  uint8_t loBot[3];     // panel, bottom of the lower ramp
  uint8_t lip[3];       // top bevel
  uint8_t shadow[3];    // bottom bevel and contact shadow
  uint8_t text[3];
  uint8_t dim[3];
  uint8_t accent[3];
  uint8_t rec[3];
  uint8_t spec;         // specular sweep strength
  uint8_t etch;         // 0 none, 1 scanline, 2 hazard hatch, 3 sigil lattice
} Skin;

static const Skin SKINS[] = {
  { "NACRE",
    {0x0a,0x0a,0x0d}, {0x3d,0x3f,0x4c}, {0x9a,0x9d,0xb4}, {0x22,0x23,0x2c},
    {0xb9,0xbd,0xd2}, {0x06,0x06,0x09}, {0xd6,0xd8,0xe4}, {0x7e,0x82,0x96},
    {0xa9,0x9a,0xe8}, {0xe8,0x78,0x8f}, 34, 0 },
  { "GUNMETAL",
    {0x08,0x09,0x0a}, {0x33,0x37,0x3a}, {0x7d,0x85,0x8b}, {0x1b,0x1d,0x1f},
    {0x9a,0xa4,0xab}, {0x04,0x05,0x06}, {0xc8,0xcd,0xd1}, {0x74,0x7c,0x82},
    {0x8f,0xb6,0xc4}, {0xd9,0x6e,0x7e}, 28, 0 },
  { "OBSIDIAN",
    {0x04,0x04,0x06}, {0x1e,0x1e,0x26}, {0x53,0x53,0x68}, {0x0e,0x0e,0x13},
    {0x6e,0x6e,0x8a}, {0x02,0x02,0x03}, {0xb6,0xb6,0xc6}, {0x63,0x63,0x76},
    {0x8e,0x7c,0xd6}, {0xcf,0x63,0x76}, 22, 0 },
  { "EMBER",
    {0x0c,0x07,0x05}, {0x44,0x2c,0x22}, {0xb2,0x77,0x55}, {0x26,0x17,0x11},
    {0xd2,0x93,0x6c}, {0x07,0x04,0x03}, {0xe6,0xd2,0xc6}, {0x96,0x76,0x66},
    {0xf0,0x9a,0x5c}, {0xe8,0x6a,0x5c}, 40, 0 },
  { "SEANCE",
    {0x09,0x06,0x0c}, {0x38,0x26,0x48}, {0x8e,0x6c,0xb0}, {0x1e,0x14,0x28},
    {0xab,0x86,0xcf}, {0x05,0x03,0x07}, {0xd8,0xcc,0xe8}, {0x8a,0x76,0xa0},
    {0xc0,0x8c,0xe8}, {0xe2,0x74,0xa8}, 36, 0 },
  { "RELIQUARY",
    {0x0b,0x09,0x06}, {0x42,0x38,0x24}, {0xb4,0x9c,0x62}, {0x24,0x1e,0x13},
    {0xd6,0xbc,0x7c}, {0x06,0x05,0x03}, {0xe8,0xdf,0xc6}, {0x96,0x8a,0x6c},
    {0xd8,0xb8,0x66}, {0xd0,0x76,0x5e}, 30, 0 },
  { "HANGAR",
    /* 2000s game-menu steel: desaturated blue-green plate, worn khaki
       readouts, machined scanlines. Nostalgic, not neon. */
    {0x09,0x0d,0x0c}, {0x36,0x41,0x3f}, {0x82,0x93,0x8e}, {0x1e,0x25,0x24},
    {0x9e,0xb0,0xaa}, {0x05,0x08,0x07}, {0xd5,0xd8,0xcd}, {0x88,0x93,0x8d},
    {0xc9,0xbb,0x92}, {0xc4,0x70,0x3f}, 26, 1 },
  { "SIGIL",
    /* Gothic cyber-sigilism. The crimson is arterial -- saturated but dark,
       so it reads as something under the surface rather than a light on it.
       Bone is the text colour, never the accent. */
    /* The horizon must stay dark: a bright waterline turns the whole panel
       pink and the red stops reading as something beneath the surface. */
    {0x04,0x02,0x07}, {0x18,0x0e,0x1a}, {0x35,0x1c,0x2c}, {0x0a,0x05,0x0c},
    {0x52,0x2a,0x3e}, {0x02,0x01,0x04}, {0xe6,0xda,0xd4}, {0x6e,0x58,0x64},
    {0xd8,0x1f,0x3c}, {0xff,0x2e,0x52}, 16, 3 },
  { "CHASSIS",
    /* Mecha plating: graphite, hazard amber used sparingly, ice blue. */
    {0x08,0x09,0x0b}, {0x30,0x36,0x40}, {0x74,0x7f,0x8e}, {0x18,0x1c,0x22},
    {0x8e,0x9a,0xa8}, {0x04,0x05,0x07}, {0xd7,0xdb,0xe0}, {0x79,0x81,0x8d},
    {0xe8,0xa3,0x3d}, {0xff,0x5c,0x3c}, 30, 2 },
};
#define NSKIN ((int)(sizeof(SKINS)/sizeof(SKINS[0])))
static int uiSkin = 0;
#define SK (SKINS[uiSkin])

#define HORIZON 0.47f        // where the ramp reverses

static uint16_t rowbuf[PW];

static inline int clamp8(int v){ return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* Ordered dither before quantising: RGB565 steps 8/4/8, so a ramp across a
   60 px panel bands into visible steps without it. The 4x4 matrix costs one
   table lookup per pixel and removes the banding entirely. */
static inline uint16_t dith(int r, int g, int b, int x, int y) {
  static const int8_t M[4][4] = {{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
  int d = M[y & 3][x & 3] - 8;                 // -8..+7
  return lcd.color565(clamp8(r + (d >> 1)),
                      clamp8(g + (d >> 2)),
                      clamp8(b + (d >> 1)));
}

static inline void mix3(uint8_t* out, const uint8_t* a, const uint8_t* b, float t) {
  for (int i = 0; i < 3; i++) out[i] = (uint8_t)(a[i] + (b[i] - a[i]) * t);
}
static inline uint16_t rgb(const uint8_t* c){ return lcd.color565(c[0],c[1],c[2]); }

/* The signature surface. Everything the device draws goes through this. */
static void wuwPanel(int x, int y, int w, int h, bool pressed) {
  if (w <= 0 || h <= 0) return;
  const uint8_t *A = pressed ? SK.loBot : SK.hiTop;
  const uint8_t *B = pressed ? SK.hiTop : SK.loBot;
  for (int r = 0; r < h; r++) {
    float t = (float)r / (h > 1 ? h - 1 : 1);
    uint8_t c[3];
    if (t < HORIZON) mix3(c, A, SK.horizon, t / HORIZON);
    else             mix3(c, SK.horizon, B, (t - HORIZON) / (1.0f - HORIZON));
    /* ONE soft diagonal sweep across the face. A modulo here tiles the
       highlight into corduroy stripes -- the band has to be positioned
       relative to the panel, not repeated across it. */
    /* The sweep is a HIGHLIGHT, not a gradient: letting its width scale with
       the panel turned any full-width surface into a bright slab with text
       floating on it. Cap the span so a large panel gets a localised catch of
       light the same size a key gets. */
    int span = (w + h) / 5; if (span < 1) span = 1; if (span > 30) span = 30;
    int centre = (w * 3 + h) / 12; if (centre > w / 2) centre = w / 2;
    /* A highlight on a large surface should be weaker as well as narrower --
       at full strength a status strip or a list row reads as a white smear
       with text floating on it. Keys keep their punch; big panels calm down. */
    int sp = SK.spec;
    if (w + h > 150) sp = sp * 150 / (w + h);
    for (int i = 0; i < w; i++) {
      int dd = (i * 3 + r) / 4 - centre; if (dd < 0) dd = -dd;
      int add = (dd < span) ? (sp * (span - dd)) / span : 0;
      /* Etch, cut in under the specular so the horizon still reads on top.
         Integer only -- this runs for every pixel of every panel. */
      int gx = x + i, gy = y + r;
      if      (SK.etch == 1) { if ((gy % 3) == 0) add -= 7; }
      else if (SK.etch == 2) { if (((gx + gy) % 11) < 2) add += 6; }
      else if (SK.etch == 3) { if (((gx * 2 + gy * 5) % 17) == 0) add += 8;
                               if (((gx * 5 - gy * 3 + 4096) % 23) == 0) add += 4; }
      rowbuf[i] = dith(c[0] + add, c[1] + add, c[2] + add, gx, gy);
    }
    lcd.pushImage(x, y + r, w, 1, rowbuf);
  }
  // rim light on the lit edges only, bevel on the others
  uint16_t lip = rgb(pressed ? SK.shadow : SK.lip);
  uint16_t sha = rgb(pressed ? SK.lip    : SK.shadow);
  lcd.drawFastHLine(x, y,         w, lip);
  lcd.drawFastHLine(x, y + 1,     w, lip);
  lcd.drawFastVLine(x, y,         h, lip);
  lcd.drawFastHLine(x, y + h - 1, w, sha);
  lcd.drawFastVLine(x + w - 1, y, h, sha);
}

/* Two shadows: a hard contact line, then a softer ambient band. One shadow
   reads as a sticker; two read as an object resting on the ground. */
static void wuwShadow(int x, int y, int w) {
  uint8_t c[3];
  mix3(c, SK.bg, SK.shadow, 0.85f);
  lcd.drawFastHLine(x, y, w, rgb(c));
  mix3(c, SK.bg, SK.shadow, 0.35f);
  lcd.drawFastHLine(x + 2, y + 1, w - 4, rgb(c));
}

static void uiColors() { /* palette lives in SKINS; kept for call compatibility */ }

static void uiKey(int i, bool down) {
  int x = i * BTN_W, y = BAR_Y, w = BTN_W - 1, h = BAR_H - 1;
  wuwPanel(x, y, w, h, down);

  const char* label; const uint8_t* col = SK.text;
  switch (i) {
    case 0: label = "SHOOT"; col = SK.accent; break;
    case 1: label = uiRec ? "STOP" : "REC"; col = uiRec ? SK.rec : SK.text; break;
    case 2: label = FX_NAME[uiFx]; col = uiFx ? SK.accent : SK.dim; break;
    case 3: label = "CARD";  break;
    default: label = SK.name; col = SK.accent; break;   // MENU shows the skin
  }
  // drop shadow under the glyphs, the way the web UI sets its labels
  lcd.setTextDatum(middle_center);
  lcd.setTextColor(rgb(SK.shadow));
  lcd.drawString(label, x + w/2, y + h/2 + 1);
  lcd.setTextColor(rgb(col));
  lcd.drawString(label, x + w/2, y + h/2);
  lcd.setTextDatum(top_left);
}

static void uiStatus(const char* mode, int fps, bool sd) {
  wuwPanel(0, 0, PW, ST_H - 1, false);
  wuwShadow(0, ST_H - 1, PW);
  lcd.setTextColor(rgb(SK.accent));
  lcd.drawString("WUW", 7, 6);
  lcd.setTextColor(rgb(SK.dim));
  lcd.drawString("CAM", 7 + 30, 6);
  char buf[44];
  snprintf(buf, sizeof(buf), "%s  %d fps  SD %s", mode, fps, sd ? "OK" : "--");
  lcd.setTextDatum(top_right);
  lcd.setTextColor(rgb(SK.dim));
  lcd.drawString(buf, PW - 7, 6);
  lcd.setTextDatum(top_left);
}

static void uiChrome(bool sd) {
  // ground: a slow vertical wash, not a flat black
  for (int r = 0; r < PH; r++) {
    uint8_t c[3];
    mix3(c, SK.bg, SK.shadow, (float)r / PH * 0.6f);
    for (int i = 0; i < PW; i++) rowbuf[i] = dith(c[0], c[1], c[2], i, r);
    lcd.pushImage(0, r, PW, 1, rowbuf);
  }
  uiStatus(FX_NAME[uiFx], 0, sd);
  for (int i = 0; i < NBTN; i++) uiKey(i, false);
  // the viewfinder well: recessed, so the image reads as sunk into the body
  lcd.drawRect(0, VF_Y - 1, PW, VF_H + 2, rgb(SK.shadow));
  lcd.drawFastHLine(0, VF_Y + VF_H + 1, PW, rgb(SK.lip));
}


/* Per-pixel looks, applied to the decoded RGB565 before it reaches the glass */
static void uiApplyFx(uint16_t* px, size_t n) {
  if (uiFx == FX_NONE) return;
  for (size_t i = 0; i < n; i++) {
    uint16_t p = px[i];
    int r = (p>>11)&0x1F, g = (p>>5)&0x3F, b = p&0x1F;
    switch (uiFx) {
      case FX_MONO: { int y = (r*77 + (g>>1)*150 + b*29) >> 8;
                      r = y; g = y<<1; b = y; } break;
      case FX_INVERT: r = 31-r; g = 63-g; b = 31-b; break;
      case FX_THERMAL: { int y = (r*77 + (g>>1)*150 + b*29) >> 8;   // 0..31
                         r = y; g = (y > 16 ? (y-16)*4 : 0); b = 31-y; } break;
      case FX_POSTER: r &= 0x18; g &= 0x30; b &= 0x18; break;
      case FX_EDGE: { int y = (r*77 + (g>>1)*150 + b*29) >> 8;
                      int d = (i>0) ? y - (int)(((px[i-1]>>11)&0x1F)) : 0;
                      d = d<0?-d:d; d = d>7?31:d*4;
                      r = 0; g = d<<1; b = d; } break;
    }
    px[i] = (uint16_t)((r<<11)|(g<<5)|b);
  }
}


/* ── Gallery: reviewing what the camera shot, on the camera ───────────────
 * Files are MJPEG in a QuickTime wrapper, which turns out to be the useful
 * property here: every frame is an independent JPEG, so playback needs no
 * container parsing and no keyframe search -- scan for SOI/EOI and decode
 * whatever falls between. Seeking is likewise just a byte offset.
 */
#define SD_CLK_PIN 39
#define SD_CMD_PIN 38
#define SD_D0_PIN  40

#define GAL_MAX 240
static char     galName[GAL_MAX][24];
static uint8_t  galVid[GAL_MAX];
static uint16_t galNum[GAL_MAX];
static int      galN = 0, galSel = 0, galTop = 0;
static bool     sdOk = false;


/* contact sheet: 3 across, 2 down, inside the viewfinder window */
#define GCOLS 3
#define GROWS 2
#define GCW   (PW / GCOLS)
#define GCH   (VF_H / GROWS)

static uint16_t* viewBuf = nullptr;      // full-screen decode target
static uint8_t*  fileBuf = nullptr;      // one JPEG, read off the card
#define FILEBUF_CAP (320 * 1024)

/* generic decode target, so thumbnails and full frames share one callback */
static uint16_t* jdBuf = nullptr; static int jdW = 0, jdH = 0;
static int jdDrawG(JPEGDRAW* p) {
  if (!jdBuf) return 0;
  for (int r = 0; r < p->iHeight; r++) {
    int py = p->y + r;
    if (py < 0 || py >= jdH) continue;
    int n = p->iWidth;
    if (p->x + n > jdW) n = jdW - p->x;
    if (n > 0) memcpy(jdBuf + (size_t)py*jdW + p->x,
                      p->pPixels + (size_t)r*p->iWidth, (size_t)n*2);
  }
  return 1;
}

static bool sdBegin() {
  if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN)) return false;
  return SD_MMC.begin("/sdcard", true);          // 1-bit mode
}

static void galScan() {
  galN = 0;
  File root = SD_MMC.open("/");
  if (!root) return;
  File f;
  while (galN < GAL_MAX && (f = root.openNextFile())) {
    if (!f.isDirectory()) {
      const char* n = f.name();
      if (*n == '/') n++;
      size_t L = strlen(n);
      bool jpg = (L > 4) && !strcasecmp(n + L - 4, ".jpg");
      bool mov = (L > 4) && !strcasecmp(n + L - 4, ".mov");
      if (jpg || mov) {
        snprintf(galName[galN], sizeof(galName[0]), "%s", n);
        galVid[galN] = mov ? 1 : 0;
        const char* u = strchr(n, '_');
        galNum[galN] = u ? (uint16_t)atoi(u + 1) : 0;
        galN++;
      }
    }
    f.close();
  }
  root.close();
  // newest first -- insertion sort, descending by file number
  for (int i = 1; i < galN; i++)
    for (int j = i; j > 0 && galNum[j] > galNum[j-1]; j--) {
      char t[24]; memcpy(t, galName[j], 24);
      memcpy(galName[j], galName[j-1], 24); memcpy(galName[j-1], t, 24);
      uint8_t v = galVid[j]; galVid[j] = galVid[j-1]; galVid[j-1] = v;
      uint16_t k = galNum[j]; galNum[j] = galNum[j-1]; galNum[j-1] = k;
    }
  Serial.printf("[GAL] %d items on card\n", galN);
}

/* Read one file (or one embedded frame) and decode it at the largest scale
   that still fits the destination. */
static bool decodeInto(uint8_t* jpg, size_t len, uint16_t* dst, int dw, int dh,
                       int* outW, int* outH) {
  if (!jdec.openRAM(jpg, len, jdDrawG)) return false;
  int w = jdec.getWidth(), h = jdec.getHeight();
  int sc = 0;
  if      (w > dw*4 || h > dh*4) sc = JPEG_SCALE_EIGHTH;
  else if (w > dw*2 || h > dh*2) sc = JPEG_SCALE_QUARTER;
  else if (w > dw   || h > dh)   sc = JPEG_SCALE_HALF;
  int div = (sc == JPEG_SCALE_EIGHTH) ? 8 : (sc == JPEG_SCALE_QUARTER) ? 4
          : (sc == JPEG_SCALE_HALF)   ? 2 : 1;
  jdW = w / div; jdH = h / div;
  if (jdW > dw) jdW = dw;
  if (jdH > dh) jdH = dh;
  jdBuf = dst;
  memset(dst, 0, (size_t)dw * dh * 2);
  jdec.setPixelType(RGB565_LITTLE_ENDIAN);
  jdec.decode(0, 0, sc);
  jdec.close();
  if (outW) *outW = jdW;
  if (outH) *outH = jdH;
  return true;
}

static size_t readFile(const char* name, uint8_t* buf, size_t cap) {
  File f = SD_MMC.open(String("/") + name);
  if (!f) return 0;
  size_t n = f.size();
  if (n > cap) n = cap;
  n = f.read(buf, n);
  f.close();
  return n;
}

static void galShowPhoto(int idx) {
  if (idx < 0 || idx >= galN || !fileBuf || !viewBuf) return;
  size_t n = readFile(galName[idx], fileBuf, FILEBUF_CAP);
  int w = 0, h = 0;
  if (!n || !decodeInto(fileBuf, n, viewBuf, PW, PH, &w, &h)) return;
  lcd.fillScreen(rgb(SK.bg));
  int oy = (PH - h) / 2, ox = (PW - w) / 2;
  for (int r = 0; r < h; r++)
    lcd.pushImage(ox, oy + r, w, 1, viewBuf + (size_t)r * PW);
  lcd.setTextDatum(bottom_left);
  lcd.setTextColor(rgb(SK.shadow)); lcd.drawString(galName[idx], 8, PH - 6);
  lcd.setTextColor(rgb(SK.text));   lcd.drawString(galName[idx], 7, PH - 7);
  lcd.setTextDatum(top_left);
}

/* MJPEG playback by marker scan: no container parsing needed, because every
   frame in the file is a self-contained JPEG. Reads in 4 KB blocks -- byte-at
   -a-time off SD_MMC would be an order of magnitude slower. */
static void galPlayVideo(int idx) {
  if (idx < 0 || idx >= galN || !fileBuf || !viewBuf) return;
  File f = SD_MMC.open(String("/") + galName[idx]);
  if (!f) return;
  lcd.fillScreen(rgb(SK.bg));
  static uint8_t blk[4096];
  size_t n = 0; bool in = false; int prev = -1; uint32_t frames = 0;
  uint32_t t0 = millis();
  while (f.available()) {
    int got = f.read(blk, sizeof(blk));
    if (got <= 0) break;
    for (int i = 0; i < got; i++) {
      int c = blk[i];
      if (!in) {
        if (prev == 0xFF && c == 0xD8) { in = true; n = 0;
                                         fileBuf[n++] = 0xFF; fileBuf[n++] = 0xD8; }
      } else {
        if (n < FILEBUF_CAP) fileBuf[n++] = (uint8_t)c;
        if (prev == 0xFF && c == 0xD9) {
          in = false;
          int w = 0, h = 0;
          if (decodeInto(fileBuf, n, viewBuf, PW, PH, &w, &h)) {
            int ox = (PW - w) / 2, oy = (PH - h) / 2;
            for (int r = 0; r < h; r++)
              lcd.pushImage(ox, oy + r, w, 1, viewBuf + (size_t)r * PW);
            frames++;
          }
          int32_t tzx, tzy;
          if (lcd.getTouch(&tzx, &tzy)) { f.close();   // tap to stop
            Serial.printf("[GAL] play stopped after %u frames\n", frames);
            return; }
        }
      }
      prev = c;
    }
  }
  f.close();
  uint32_t ms = millis() - t0;
  Serial.printf("[GAL] played %u frames in %u ms (%.1f fps)\n",
                frames, ms, ms ? frames * 1000.0f / ms : 0.0f);
}

/* ── WUW CAM OS ───────────────────────────────────────────────────────────
 * Ported from the simulator, which was written to make this mechanical: the
 * same layout constants, the same screen enum, the same draw order. Panels go
 * through wuwPanel(); icons that were canvas paths there are LovyanGFX
 * primitives here.
 *
 * ONE RULE worth restating, because breaking it is what made three screens
 * unreadable during design: SK.dim is for text on the dark GROUND only. On a
 * lit panel it lands within a few percent of the panel's own mid-tone and
 * disappears. Secondary text on a panel uses inkOn() and carries a shadow.
 */
#define OS_ST_H   20
#define OS_BAR_H  52
#define OS_BAR_Y  (PH - OS_BAR_H)
#define OS_BODY_Y OS_ST_H
#define OS_BODY_H (OS_BAR_Y - OS_ST_H)

enum { S_HOME = 0, S_LOOKS, S_TUNE, S_CARD, S_MORE,
       S_PRESET, S_ROOM, S_SET, S_SYS, S_CHAT, S_COUNT };
static const char* SCREEN_NAME[S_COUNT] =
  { "VIEW","LOOKS","TUNE","CARD","MORE","PRESETS","ROOM","SETTINGS","SYSTEM","CHAT" };

static int  osScreen = S_HOME;
static int  osPage = 0, osSel = 0;
static int8_t osHeld = -1;

/* Effect parameters the panel can actually change. These mirror the web UI's
   names so a preset written by one is readable by the other. */
static float osP[6] = { 0.55f, 0.50f, 0.45f, 0.30f, 1.00f, 1.00f };
static const char* OS_PNAME[6] = { "AMOUNT","DEPTH","SPEED","GRAIN","MIX","ZOOM" };

static uint16_t rgb565(const uint8_t* c) { return lcd.color565(c[0], c[1], c[2]); }
static uint16_t inkOn() {                       // readable secondary on a panel
  uint8_t c[3];
  for (int i = 0; i < 3; i++)
    c[i] = (uint8_t)(SK.dim[i] + (SK.text[i] - SK.dim[i]) * 0.72f);
  return lcd.color565(c[0], c[1], c[2]);
}
static void osText(const char* s, int x, int y, uint16_t col,
                   uint8_t datum = textdatum_t::top_left, bool shadow = true) {
  lcd.setTextDatum((textdatum_t)datum);
  if (shadow) { lcd.setTextColor(rgb565(SK.shadow)); lcd.drawString(s, x, y + 1); }
  lcd.setTextColor(col);
  lcd.drawString(s, x, y);
  lcd.setTextDatum(top_left);
}

/* ── icons ───────────────────────────────────────────────────────────────
   Line art at 16 px. Deliberately simple shapes: at this size on a 320x240
   panel, anything more detailed turns to mud. */
static void osIcon(const char* k, int x, int y, uint16_t col, int sz = 16) {
  float u = sz / 16.0f;
  auto X = [&](float v) { return (int)(x + v * u); };
  auto Y = [&](float v) { return (int)(y + v * u); };
  if      (!strcmp(k, "aperture")) {
    lcd.drawCircle(X(8), Y(8), (int)(6 * u), col);
    for (int i = 0; i < 6; i++) { float a = i * 1.047f;
      lcd.drawLine(X(8 + cosf(a) * 2), Y(8 + sinf(a) * 2),
                   X(8 + cosf(a) * 6), Y(8 + sinf(a) * 6), col); }
  } else if (!strcmp(k, "sliders")) {
    for (int i = 0; i < 3; i++) lcd.drawFastHLine(X(2), Y(4 + i * 4), (int)(12 * u), col);
    lcd.fillCircle(X(5),  Y(4),  (int)(2 * u), col);
    lcd.fillCircle(X(10), Y(8),  (int)(2 * u), col);
    lcd.fillCircle(X(6),  Y(12), (int)(2 * u), col);
  } else if (!strcmp(k, "grid")) {
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++)
      lcd.drawRect(X(2 + a * 7), Y(2 + b * 7), (int)(5 * u), (int)(5 * u), col);
  } else if (!strcmp(k, "card")) {
    lcd.drawRect(X(2), Y(3), (int)(12 * u), (int)(10 * u), col);
    lcd.drawLine(X(2), Y(10), X(6), Y(6), col);
    lcd.drawLine(X(6), Y(6), X(10), Y(10), col);
  } else if (!strcmp(k, "more")) {
    lcd.fillCircle(X(3), Y(8), (int)(1.4f * u), col);
    lcd.fillCircle(X(8), Y(8), (int)(1.4f * u), col);
    lcd.fillCircle(X(13), Y(8), (int)(1.4f * u), col);
  } else if (!strcmp(k, "gear")) {
    lcd.drawCircle(X(8), Y(8), (int)(3.2f * u), col);
    for (int i = 0; i < 8; i++) { float a = i * 0.785f;
      lcd.drawLine(X(8 + cosf(a) * 5), Y(8 + sinf(a) * 5),
                   X(8 + cosf(a) * 6.8f), Y(8 + sinf(a) * 6.8f), col); }
  } else if (!strcmp(k, "people")) {
    lcd.drawCircle(X(5.5f), Y(6), (int)(2.3f * u), col);
    lcd.drawCircle(X(11), Y(6.5f), (int)(1.8f * u), col);
    lcd.drawLine(X(2), Y(13), X(5.5f), Y(9.5f), col);
    lcd.drawLine(X(5.5f), Y(9.5f), X(9), Y(13), col);
  } else if (!strcmp(k, "save")) {
    lcd.drawRect(X(2.5f), Y(2.5f), (int)(11 * u), (int)(11 * u), col);
    lcd.drawRect(X(5), Y(9.5f), (int)(6 * u), (int)(4 * u), col);
    lcd.drawFastHLine(X(5), Y(3), (int)(6 * u), col);
  } else if (!strcmp(k, "chip")) {
    lcd.drawRect(X(4), Y(4), (int)(8 * u), (int)(8 * u), col);
    for (int i = 0; i < 3; i++) { float o = 4.5f + i * 3.5f;
      lcd.drawLine(X(o), Y(4), X(o), Y(1.5f), col);
      lcd.drawLine(X(o), Y(12), X(o), Y(14.5f), col);
      lcd.drawLine(X(4), Y(o), X(1.5f), Y(o), col);
      lcd.drawLine(X(12), Y(o), X(14.5f), Y(o), col); }
  } else if (!strcmp(k, "chat")) {
    lcd.drawRect(X(2), Y(4), (int)(12 * u), (int)(7 * u), col);
    lcd.drawLine(X(4), Y(11), X(4), Y(14), col);
    lcd.drawLine(X(4), Y(14), X(7), Y(11), col);
  } else if (!strcmp(k, "rec")) {
    lcd.fillCircle(X(8), Y(8), (int)(5 * u), col);
  } else if (!strcmp(k, "back")) {
    lcd.drawLine(X(10), Y(3), X(5), Y(8), col);
    lcd.drawLine(X(5), Y(8), X(10), Y(13), col);
  } else if (!strcmp(k, "mask")) {
    lcd.drawCircle(X(8), Y(8), (int)(6 * u), col);
    lcd.fillArc(X(8), Y(8), 0, (int)(6 * u), 270, 90, col);
  } else if (!strcmp(k, "box")) {
    lcd.drawRect(X(2.5f), Y(4), (int)(11 * u), (int)(8 * u), col);
  } else if (!strcmp(k, "wifi")) {
    for (int i = 0; i < 3; i++)
      lcd.drawArc(X(8), Y(12), (int)((3 + i * 3) * u), (int)((3 + i * 3) * u) + 1,
                  200, 340, col);
    lcd.fillCircle(X(8), Y(12), (int)(1.2f * u), col);
  }
}

/* ── status strip ───────────────────────────────────────────────────────── */
static void osStatus() {
  wuwPanel(0, 0, PW, OS_ST_H - 1, false);
  uint8_t c[3];
  for (int i = 0; i < 3; i++) c[i] = (uint8_t)(SK.bg[i] + (SK.shadow[i] - SK.bg[i]) * 0.85f);
  lcd.drawFastHLine(0, OS_ST_H - 1, PW, lcd.color565(c[0], c[1], c[2]));

  osText("WUW", 6, 4, rgb565(SK.accent));
  osText(SCREEN_NAME[osScreen], 34, 4, inkOn());
  char right[32];
  snprintf(right, sizeof(right), "%s  %s", camOK ? "CAM" : "---", sdOk ? "SD" : "--");
  osText(right, PW - 7, 4, inkOn(), textdatum_t::top_right);
}

/* ── contextual key bar ─────────────────────────────────────────────────── */
struct BarKey { const char* icon; const char* label; };
static void osBarKeys(BarKey* out) {
  static const BarKey home[5]  = {{"aperture","SHOOT"},{"rec","REC"},{"grid","LOOKS"},
                                  {"sliders","TUNE"},{"more","MORE"}};
  static const BarKey looks[5] = {{"back","BACK"},{"grid","PAGE"},{"aperture","APPLY"},
                                  {"mask","MASK"},{"box","TRACK"}};
  static const BarKey tune[5]  = {{"back","BACK"},{"sliders","RESET"},{"aperture","SHOOT"},
                                  {"save","SAVE"},{"more","MORE"}};
  static const BarKey card[5]  = {{"back","BACK"},{"card","OPEN"},{"aperture","SHOOT"},
                                  {"grid","PAGE"},{"more","MORE"}};
  static const BarKey more[5]  = {{"back","BACK"},{"chat","CHAT"},{"save","PRESETS"},
                                  {"people","ROOM"},{"gear","SET"}};
  static const BarKey leaf[5]  = {{"back","BACK"},{"",""},{"aperture","SHOOT"},
                                  {"",""},{"more","MORE"}};
  const BarKey* src = leaf;
  switch (osScreen) {
    case S_HOME:  src = home;  break;
    case S_LOOKS: src = looks; break;
    case S_TUNE:  src = tune;  break;
    case S_CARD:  src = card;  break;
    case S_MORE:  src = more;  break;
    default: break;
  }
  for (int i = 0; i < 5; i++) out[i] = src[i];
}
static void osBar() {
  BarKey k[5]; osBarKeys(k);
  for (int i = 0; i < NBTN; i++)
    wuwPanel(i * BTN_W, OS_BAR_Y, BTN_W - 1, OS_BAR_H - 1, i == osHeld);
  for (int i = 0; i < NBTN; i++) {
    if (!k[i].label[0]) continue;
    int cx = i * BTN_W + (BTN_W - 1) / 2;
    uint16_t col = (i == 0 && osScreen == S_HOME) ? rgb565(SK.accent)
                 : (i == 1 && uiRec)              ? rgb565(SK.rec)
                 : (!strcmp(k[i].label, "BACK"))  ? inkOn()
                 : rgb565(SK.text);
    if (k[i].icon[0]) osIcon(k[i].icon, cx - 8, OS_BAR_Y + 9, col, 16);
    osText(i == 1 && uiRec ? "STOP" : k[i].label, cx, OS_BAR_Y + 30, col,
           textdatum_t::top_center);
  }
}

/* ── screens ─────────────────────────────────────────────────────────────── */
static void osGround(int y0, int y1, float t) {
  uint8_t c[3];
  for (int i = 0; i < 3; i++) c[i] = (uint8_t)(SK.bg[i] + (SK.shadow[i] - SK.bg[i]) * t);
  for (int r = y0; r < y1; r++) {
    for (int i = 0; i < PW; i++) rowbuf[i] = dith(c[0], c[1], c[2], i, r);
    lcd.pushImage(0, r, PW, 1, rowbuf);
  }
}

static void osHomeHud() {
  char buf[40];
  lcd.setTextDatum(top_left);
  snprintf(buf, sizeof(buf), "%s", FX_NAME[uiFx]);
  osText(buf, 6, OS_BODY_Y + 3, rgb565(SK.accent));
  osText(sdOk ? "SD OK" : "NO CARD", PW - 6, OS_BODY_Y + 3,
         sdOk ? inkOn() : rgb565(SK.rec), textdatum_t::top_right);
  if (uiRec) {
    lcd.fillCircle(12, OS_BODY_Y + 26, 4, rgb565(SK.rec));
    osText("REC", 22, OS_BODY_Y + 21, rgb565(SK.rec));
  }
  char n[24];
  snprintf(n, sizeof(n), "%d ON CARD", galN);
  osText(n, PW - 6, OS_BODY_Y + OS_BODY_H - 13, inkOn(), textdatum_t::top_right);
}

#define LOOKS_COLS 4
#define LOOKS_ROWS 2
static void osLooks() {
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.40f);
  int CW = PW / LOOKS_COLS, CH = OS_BODY_H / LOOKS_ROWS;
  for (int k = 0; k < LOOKS_COLS * LOOKS_ROWS; k++) {
    int i = osPage * (LOOKS_COLS * LOOKS_ROWS) + k;
    if (i >= FX_COUNT) break;
    int bx = (k % LOOKS_COLS) * CW, by = OS_BODY_Y + (k / LOOKS_COLS) * CH;
    bool on = (i == uiFx);
    wuwPanel(bx + 2, by + 2, CW - 4, CH - 4, on);
    osIcon("aperture", bx + CW / 2 - 8, by + 9, on ? rgb565(SK.accent) : inkOn(), 16);
    osText(FX_NAME[i], bx + CW / 2, by + CH - 21,
           on ? rgb565(SK.accent) : rgb565(SK.text), textdatum_t::top_center);
  }
  char p[20];
  snprintf(p, sizeof(p), "PAGE %d/%d", osPage + 1,
           (FX_COUNT + 7) / (LOOKS_COLS * LOOKS_ROWS));
  osText(p, PW / 2, OS_BODY_Y + OS_BODY_H - 11, inkOn(), textdatum_t::top_center);
}

static void osTuneGeom(int i, int* y, int* h, int* tx, int* tw, int* ty) {
  int rowH = (OS_BODY_H - 8) / 6;
  *y = OS_BODY_Y + 4 + i * rowH; *h = rowH;
  *tx = 96; *tw = PW - 96 - 14; *ty = *y + rowH / 2 - 3;
}
static void osTune() {
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.35f);
  for (int i = 0; i < 6; i++) {
    int y, h, tx, tw, ty; osTuneGeom(i, &y, &h, &tx, &tw, &ty);
    wuwPanel(tx, ty, tw, 7, true);                       // recessed track
    float v = (i == 5) ? (osP[5] - 1.0f) / 2.0f : osP[i];
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    int fw = (int)((tw - 4) * v); if (fw < 4) fw = 4;
    wuwPanel(tx + 2, ty + 1, fw, 5, false);              // the fill sits proud
    osText(OS_PNAME[i], 10, y + h / 2 - 8, rgb565(SK.text));
    char val[10];
    snprintf(val, sizeof(val), "%.2f", (i == 5) ? osP[5] : osP[i]);
    osText(val, 90, y + h / 2 - 8, rgb565(SK.accent), textdatum_t::top_right);
  }
}

struct ListRow { const char* icon; const char* label; const char* sub; int go; };
static void osList(const ListRow* rows, int n) {
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.35f);
  int rowH = (OS_BODY_H - 6) / (n > 0 ? n : 1);
  for (int i = 0; i < n; i++) {
    int y = OS_BODY_Y + 3 + i * rowH;
    wuwPanel(6, y, PW - 12, rowH - 4, i == osSel);
    osIcon(rows[i].icon, 16, y + rowH / 2 - 12,
           i == osSel ? rgb565(SK.accent) : inkOn(), 16);
    osText(rows[i].label, 42, y + rowH / 2 - 13,
           i == osSel ? rgb565(SK.accent) : rgb565(SK.text));
    osText(rows[i].sub, 42, y + rowH / 2 + 1, inkOn());
  }
}
static const ListRow MORE_ROWS[] = {
  {"chat","CHAT","post to the PASAR room", S_CHAT},
  {"save","PRESETS","saved looks on the card", S_PRESET},
  {"people","ROOM","visitors and session", S_ROOM},
  {"gear","SETTINGS","skin, look, tracking", S_SET},
  {"chip","SYSTEM","memory, storage, uptime", S_SYS},
};
#define MORE_N ((int)(sizeof(MORE_ROWS)/sizeof(MORE_ROWS[0])))

static void osInfoPanel() {
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.35f);
  wuwPanel(6, OS_BODY_Y + 4, PW - 12, OS_BODY_H - 10, true);   // recessed: readable
}
static void osKV(const char* k, const char* v, int i) {
  int y = OS_BODY_Y + 14 + i * 17;
  osText(k, 16, y, inkOn());
  osText(v, PW - 18, y, rgb565(SK.text), textdatum_t::top_right);
}
static void osSettings() {
  osInfoPanel();
  char b[28];
  osKV("SKIN", SK.name, 0);
  osKV("LOOK", FX_NAME[uiFx], 1);
  snprintf(b, sizeof(b), "%d", galN);       osKV("ON CARD", b, 2);
  osKV("CAMERA", camOK ? "ready" : "offline", 3);
  osKV("CARD",   sdOk  ? "mounted" : "absent", 4);
}
static void osSystem() {
  osInfoPanel();
  char b[28];
  osKV("FIRMWARE", "WUW 1.0.0", 0);
  snprintf(b, sizeof(b), "%u KB", (unsigned)(ESP.getFreeHeap() / 1024));
  osKV("HEAP", b, 1);
  snprintf(b, sizeof(b), "%u KB", (unsigned)(ESP.getFreePsram() / 1024));
  osKV("PSRAM", b, 2);
  uint32_t up = millis() / 1000;
  snprintf(b, sizeof(b), "%luh %lum", (unsigned long)(up / 3600),
           (unsigned long)((up % 3600) / 60));
  osKV("UPTIME", b, 3);
  osKV("PANEL", "320x240", 4);
}
static void osRoom() {
  osInfoPanel();
  osIcon("people", 14, OS_BODY_Y + 14, rgb565(SK.accent), 22);
  osText("STANDALONE", 46, OS_BODY_Y + 14, rgb565(SK.text));
  osText("no network in this build", 46, OS_BODY_Y + 30, inkOn());
  osKV("CARD", sdOk ? "mounted" : "absent", 3);
  char b[24]; snprintf(b, sizeof(b), "%d", galN);
  osKV("IMAGES", b, 4);
}
static void osPresets() {
  static ListRow rows[4];
  int n = 0;
  rows[n++] = {"save","(none yet)","save a look from TUNE", S_MORE};
  osList(rows, n);
}

static void osCard() {
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.40f);
  int CW = PW / 3, CH = OS_BODY_H / 2;
  for (int k = 0; k < 6; k++) {
    int idx = galTop + k;
    int bx = (k % 3) * CW, by = OS_BODY_Y + (k / 3) * CH;
    wuwPanel(bx + 2, by + 2, CW - 4, CH - 4, idx == galSel);
    if (idx >= galN) continue;
    if (fileBuf && viewBuf) {
      size_t nb = readFile(galName[idx], fileBuf, FILEBUF_CAP);
      int tw = 0, th = 0;
      if (nb && decodeInto(fileBuf, nb, viewBuf, CW - 12, CH - 22, &tw, &th)) {
        int ox = bx + (CW - tw) / 2, oy = by + (CH - 18 - th) / 2;
        for (int r = 0; r < th; r++)
          lcd.pushImage(ox, oy + r, tw, 1, viewBuf + (size_t)r * (CW - 12));
      }
    }
    osText(galName[idx], bx + CW / 2, by + CH - 17,
           idx == galSel ? rgb565(SK.accent) : inkOn(), textdatum_t::top_center);
    if (galVid[idx])
      lcd.fillTriangle(bx + 8, by + 8, bx + 8, by + 17, bx + 16, by + 12.5f,
                       rgb565(SK.accent));
  }
}

static void osDraw() {
  osStatus();
  switch (osScreen) {
    case S_HOME:   /* the live frame is pushed by the render loop */ break;
    case S_LOOKS:  osLooks();    break;
    case S_TUNE:   osTune();     break;
    case S_CARD:   osCard();     break;
    case S_MORE:   osList(MORE_ROWS, MORE_N); break;
    case S_PRESET: osPresets();  break;
    case S_ROOM:   osRoom();     break;
    case S_SET:    osSettings(); break;
    case S_SYS:    osSystem();   break;
    default: osInfoPanel(); break;
  }
  if (osScreen == S_HOME) osHomeHud();
  osBar();
}


/* ── touch ───────────────────────────────────────────────────────────────
   Same hit regions and the same resulting state changes as the simulator.
   Key presses act on RELEASE so the sunken key is visible while held; body
   taps act immediately, which is what a slider needs. */
static bool     osDown = false;
static int      osDragRow = -1;
static uint32_t osRepaint = 1;          // set whenever the screen must redraw

static void osGo(int screen) { osScreen = screen; osSel = 0; osPage = 0; osRepaint = 1; }

static void osKeyAction(int i) {
  switch (osScreen) {
    case S_HOME:
      if      (i == 0) { Serial.println("[OS] shutter"); }
      else if (i == 1) { uiRec = !uiRec; osRepaint = 1; }
      else if (i == 2) osGo(S_LOOKS);
      else if (i == 3) osGo(S_TUNE);
      else if (i == 4) osGo(S_MORE);
      break;
    case S_LOOKS:
      if      (i == 0) osGo(S_HOME);
      else if (i == 1) { osPage = (osPage + 1) % ((FX_COUNT + 7) / 8); osRepaint = 1; }
      else if (i == 2) osGo(S_HOME);
      else             osRepaint = 1;
      break;
    case S_TUNE:
      if      (i == 0) osGo(S_HOME);
      else if (i == 1) { osP[0]=.55f; osP[1]=.5f; osP[2]=.45f;
                         osP[3]=.3f; osP[4]=1.f; osP[5]=1.f; osRepaint = 1; }
      else if (i == 3) osGo(S_PRESET);
      else if (i == 4) osGo(S_MORE);
      break;
    case S_CARD:
      if      (i == 0) osGo(S_HOME);
      else if (i == 1) { if (galSel < galN) {
                           if (galVid[galSel]) galPlayVideo(galSel);
                           else                galShowPhoto(galSel);
                           osRepaint = 1; } }
      else if (i == 3) { galTop = (galTop + 6 < galN) ? galTop + 6 : 0; osRepaint = 1; }
      else if (i == 4) osGo(S_MORE);
      break;
    case S_MORE:
      if (i == 0) osGo(S_HOME);
      else if (i - 1 < MORE_N) osGo(MORE_ROWS[i - 1].go);
      break;
    default:
      if      (i == 0) osGo(S_MORE);
      else if (i == 4) osGo(S_MORE);
      break;
  }
}

static void osBodyTap(int x, int y) {
  if (osScreen == S_LOOKS) {
    int CW = PW / LOOKS_COLS, CH = OS_BODY_H / LOOKS_ROWS;
    int k = ((y - OS_BODY_Y) / CH) * LOOKS_COLS + (x / CW);
    int i = osPage * (LOOKS_COLS * LOOKS_ROWS) + k;
    if (i >= 0 && i < FX_COUNT) { uiFx = i; osRepaint = 1; }
  } else if (osScreen == S_TUNE) {
    for (int i = 0; i < 6; i++) {
      int yy, h, tx, tw, ty; osTuneGeom(i, &yy, &h, &tx, &tw, &ty);
      if (y >= yy && y < yy + h) {
        float v = (float)(x - tx - 2) / (float)(tw - 4);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        osP[i] = (i == 5) ? 1.0f + v * 2.0f : v;
        osDragRow = i; osRepaint = 1; return;
      }
    }
  } else if (osScreen == S_CARD) {
    int CW = PW / 3, CH = OS_BODY_H / 2;
    int k = ((y - OS_BODY_Y) / CH) * 3 + (x / CW);
    if (galTop + k < galN) { galSel = galTop + k; osRepaint = 1; }
  } else if (osScreen == S_MORE) {
    int rowH = (OS_BODY_H - 6) / MORE_N;
    int i = (y - OS_BODY_Y - 3) / rowH;
    if (i >= 0 && i < MORE_N) osGo(MORE_ROWS[i].go);
  }
}

static void osTouch() {
  if (!lcdOK) return;
  int32_t x, y;
  bool down = lcd.getTouch(&x, &y);
  if (down && !osDown) {                                   // press
    osDown = true;
    if (y >= OS_BAR_Y) {
      osHeld = x / BTN_W; if (osHeld >= NBTN) osHeld = NBTN - 1;
      osRepaint = 1;
    } else if (y >= OS_BODY_Y) {
      osBodyTap(x, y);
    }
  } else if (down && osDown && osDragRow >= 0 && osScreen == S_TUNE) {
    int yy, h, tx, tw, ty; osTuneGeom(osDragRow, &yy, &h, &tx, &tw, &ty);
    float v = (float)(x - tx - 2) / (float)(tw - 4);
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    osP[osDragRow] = (osDragRow == 5) ? 1.0f + v * 2.0f : v;
    osRepaint = 1;
  } else if (!down && osDown) {                            // release
    osDown = false; osDragRow = -1;
    if (osHeld >= 0) { int k = osHeld; osHeld = -1; osRepaint = 1; osKeyAction(k); }
  }
}

static bool initCamera(int mode) {
  camera_config_t c = {};
  c.ledc_channel=LEDC_CHANNEL_0; c.ledc_timer=LEDC_TIMER_0;
  c.pin_d0=Y2_GPIO_NUM; c.pin_d1=Y3_GPIO_NUM; c.pin_d2=Y4_GPIO_NUM; c.pin_d3=Y5_GPIO_NUM;
  c.pin_d4=Y6_GPIO_NUM; c.pin_d5=Y7_GPIO_NUM; c.pin_d6=Y8_GPIO_NUM; c.pin_d7=Y9_GPIO_NUM;
  c.pin_xclk=XCLK_GPIO_NUM; c.pin_pclk=PCLK_GPIO_NUM;
  c.pin_vsync=VSYNC_GPIO_NUM; c.pin_href=HREF_GPIO_NUM;
  c.pin_sccb_sda=SIOD_GPIO_NUM; c.pin_sccb_scl=SIOC_GPIO_NUM;
  c.pin_pwdn=-1; c.pin_reset=-1;
  c.xclk_freq_hz=20000000;
  c.grab_mode=CAMERA_GRAB_LATEST; c.fb_location=CAMERA_FB_IN_PSRAM;
  if (mode == MODE_RGB565) {
    // Panel-format pixels straight off the sensor: no JPEG, nothing to decode.
    c.pixel_format=PIXFORMAT_RGB565; c.frame_size=FRAMESIZE_QVGA;  // exactly 320x240
    c.fb_count=2;
  } else {
    /* Measured (rgbbench, on this board): capturing VGA and decoding at half
       scale costs 60 ms, because the decoder still parses every MCU of a frame
       we throw three quarters of. Capturing QVGA natively and decoding 1:1 is
       the same 320x240 output for 27 ms. */
    c.pixel_format=PIXFORMAT_JPEG;   c.frame_size=FRAMESIZE_QVGA;  // native panel size
    c.jpeg_quality=12; c.fb_count=2;
  }
  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) { Serial.printf("[CAM] init 0x%x (%s)\n", e,
                                   mode==MODE_RGB565?"RGB565":"JPEG"); return false; }
  camMode = mode;
  return true;
}

/* Core-0 producer: grab, convert to panel format, publish. Never touches the
   panel or the SPI bus, so it cannot race LovyanGFX on core 1. */
static void capTaskFn(void*) {
  int w = 0;
  for (;;) {
    if (wantMode != camMode) {                 // MENU asked for the other path
      esp_camera_deinit();
      if (!initCamera(wantMode)) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
      capReady = -1;
      Serial.printf("[CAM] now %s\n", camMode==MODE_RGB565?"RGB565 direct":"JPEG + decode");
    }
    uint32_t t0 = (uint32_t)esp_timer_get_time();
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    uint32_t t1 = (uint32_t)esp_timer_get_time();
    if (camMode == MODE_RGB565) {
      size_t n = fb->len < (size_t)PW*PH*2 ? fb->len : (size_t)PW*PH*2;
      memcpy(capBuf[w], fb->buf, n);
    } else {
      jdTarget = capBuf[w];
      if (jdec.openRAM(fb->buf, fb->len, jdDraw)) {
        jdec.setPixelType(RGB565_LITTLE_ENDIAN);
        jdec.decode(0, 0, 0);            // 1:1 — the frame is already 320x240
        jdec.close();
      }
    }
    uint32_t t2 = (uint32_t)esp_timer_get_time();
    esp_camera_fb_return(fb);
    capUs = t1-t0; capDecUs = t2-t1;
    xSemaphoreTake(capMux, portMAX_DELAY);
    capReady = w; capSeq++;
    xSemaphoreGive(capMux);
    w ^= 1;                                    // fill the other slot next
  }
}

void setup() {
  Serial.begin(115200);
  delay(600);
  setCpuFrequencyMhz(240);
  Serial.println("\n== WUW CAM — panel bring-up (JPEGDEC + changed-tile push) ==");

  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);        // backlight on; PWM here for brightness
  lcdOK = lcd.init();
  Serial.printf("[LCD] init %s\n", lcdOK ? "OK" : "FAILED — check wiring and 3V3");
  if (lcdOK) {
    lcd.setRotation(1);
    uiColors();
    uiChrome(false);
    Serial.printf("[LCD] %dx%d — pocket UI up\n", lcd.width(), lcd.height());
  }

  camOK = initCamera(MODE_JPEG);
  cur = (uint16_t*)heap_caps_malloc(PW*PH*2, MALLOC_CAP_SPIRAM);
  ret = (uint16_t*)heap_caps_malloc(PW*PH*2, MALLOC_CAP_SPIRAM);
  run = (uint16_t*)heap_caps_malloc(PW*TILE*2, MALLOC_CAP_INTERNAL);
  capBuf[0] = (uint16_t*)heap_caps_malloc(PW*PH*2, MALLOC_CAP_SPIRAM);
  capBuf[1] = (uint16_t*)heap_caps_malloc(PW*PH*2, MALLOC_CAP_SPIRAM);
  if (ret) memset(ret, 0, PW*PH*2);
  Serial.printf("[CAM] %s | buffers %s\n", camOK?"OK":"OFFLINE",
                (cur&&ret&&run&&capBuf[0]&&capBuf[1])?"ok":"FAILED");

  sdOk = sdBegin();
  Serial.printf("[SD] %s\n", sdOk ? "OK" : "not mounted");
  viewBuf = (uint16_t*)heap_caps_malloc((size_t)PW*PH*2, MALLOC_CAP_SPIRAM);
  fileBuf = (uint8_t*) heap_caps_malloc(FILEBUF_CAP,      MALLOC_CAP_SPIRAM);
  Serial.printf("[GAL] buffers %s\n", (viewBuf && fileBuf) ? "ok" : "FAILED");
  if (sdOk) galScan();

  capMux = xSemaphoreCreateMutex();
  if (camOK && capMux && capBuf[0] && capBuf[1])
    xTaskCreatePinnedToCore(capTaskFn, "cap", 6144, nullptr, 4, nullptr, 0);
  Serial.println("[TASK] capture pinned to core 0, render + panel on core 1");
  Serial.println("[UI]   MENU switches JPEG+decode <-> RGB565 direct\n");
}

void loop() {
  static uint32_t n=0;
  static uint64_t aDec=0, aDiff=0, aFull=0, aChg=0, aQtr=0;
  static uint32_t chgAcc=0, cFull=0, cChg=0, first=1;
  static uint32_t worstLoop=0;
  uint64_t tLoop = esp_timer_get_time();

  osTouch();
  if (osRepaint) { osRepaint = 0; osDraw(); }

  /* Only the viewfinder wants the camera. Every other screen leaves the
     sensor alone, which also keeps the panel still while someone reads it. */
  if (osScreen != S_HOME) { delay(20); return; }

  if (!camOK || !lcdOK || !cur || !ret || !run) { delay(400); return; }

  // Take whatever core 0 finished last. If nothing new has landed we return
  // immediately and poll touch again, so input never waits on the sensor.
  static uint32_t lastSeq = 0;
  if (capReady < 0 || capSeq == lastSeq) { delay(1); return; }
  uint64_t t = esp_timer_get_time();
  xSemaphoreTake(capMux, portMAX_DELAY);
  memcpy(cur, capBuf[capReady], (size_t)PW*PH*2);
  lastSeq = capSeq;
  xSemaphoreGive(capMux);
  aDec += esp_timer_get_time() - t;

  // ── which tiles actually moved (luma, not packed bits) ──
  t = esp_timer_get_time();
  uint32_t changed = 0;
  for (int ty2=0; ty2<TY; ty2++) {
    for (int tx2=0; tx2<TX; tx2++) {
      uint32_t d=0, s=0;
      for (int r=0; r<TILE; r+=4) {
        uint16_t* a = cur + (size_t)(ty2*TILE+r)*PW + tx2*TILE;
        uint16_t* b = ret + (size_t)(ty2*TILE+r)*PW + tx2*TILE;
        for (int i=0;i<TILE;i+=2){ int v=luma565(a[i])-luma565(b[i]);
                                   d += (v<0?-v:v); s++; }
      }
      bool ch = first || (s && (d/s) > 8);
      dirty[ty2][tx2] = ch;
      if (ch) changed++;
    }
  }
  aDiff += esp_timer_get_time() - t;
  chgAcc += changed;

  // apply the on-device look before anything reaches the panel
  uiApplyFx(cur, (size_t)PW * PH);

  bool doFull = (n & 1);
  t = esp_timer_get_time();
  if (doFull) {
    // centre-crop the 4:3 frame into the viewfinder window
    for (int r = 0; r < VF_H; r++)
      lcd.pushImage(0, VF_Y + r, PW, 1, cur + (size_t)((PH-VF_H)/2 + r) * PW);
    aFull += esp_timer_get_time() - t; cFull++;
  } else {
    // Coalesce horizontally adjacent dirty tiles into runs — one address
    // window per run instead of one per tile, which is where the real
    // per-call SPI overhead lives.
    for (int ty2=0; ty2<TY; ty2++) {
      int x = 0;
      while (x < TX) {
        if (!dirty[ty2][x]) { x++; continue; }
        int x0 = x;
        while (x < TX && dirty[ty2][x]) x++;
        int wpx = (x - x0) * TILE, xpx = x0 * TILE, ypx = ty2 * TILE;
        int crop = (PH - VF_H) / 2;
        if (ypx < crop || ypx + TILE > crop + VF_H) continue;   // outside window
        for (int r=0; r<TILE; r++)
          memcpy(run + (size_t)r*wpx, cur + (size_t)(ypx+r)*PW + xpx, (size_t)wpx*2);
        lcd.pushImage(xpx, VF_Y + (ypx - crop), wpx, TILE, run);
      }
    }
    aChg += esp_timer_get_time() - t; cChg++;
  }

  { uint16_t* sw = ret; ret = cur; cur = sw; }   // what we drew is now the retina
  first = 0;

  {
    uint32_t lp = (uint32_t)(esp_timer_get_time() - tLoop);
    if (lp > worstLoop) worstLoop = lp;          // bounds touch latency
  }
  if (++n % 20 == 0 && cFull && cChg) {
    float fetch=aDec/20000.0f, dif=aDiff/20000.0f;
    float full=aFull/(float)cFull/1000.0f, chg=aChg/(float)cChg/1000.0f;
    float pct = 100.0f*chgAcc/20.0f/(TX*TY);
    float cap=capUs/1000.0f, dc=capDecUs/1000.0f;
    const char* mn = (camMode==MODE_RGB565) ? "RGB565 direct" : "JPEG + decode";
    uiStatus(FX_NAME[uiFx], (int)(1000.0f/(fetch+dif+chg)), true);
    Serial.printf("[%s]\n", mn);
    Serial.printf("  core0  capture %.1f  decode %.1f ms\n", cap, dc);
    Serial.printf("  core1  fetch %.1f  diff %.1f  push-full %.1f  push-changed %.1f ms"
                  " | dirty %.0f%%\n", fetch, dif, full, chg, pct);
    // Core 0 and core 1 run concurrently, so the frame time is whichever
    // side is slower, not the sum of both.
    float c0 = cap + dc, c1f = fetch+dif+full, c1c = fetch+dif+chg;
    float pf = c0 > c1f ? c0 : c1f, pc = c0 > c1c ? c0 : c1c;
    Serial.printf("  frame  full %.1f ms (%.1f fps) | changed %.1f ms (%.1f fps)\n",
                  pf, 1000.0f/pf, pc, 1000.0f/pc);
    Serial.printf("  touch  worst response %.1f ms\n\n", worstLoop/1000.0f);
    aDec=aDiff=aFull=aChg=aQtr=0; cFull=cChg=0; chgAcc=0; worstLoop=0;
  }
}
