/* ── WUW CAM panel ────────────────────────────────────────────────────────
 * The device OS, extracted from lcdtest.cpp so it can live in the SAME binary
 * as the camera, the web server and PASAR. That merge is the point: a screen
 * showing who is in the room, and a keyboard that can post to it, cannot work
 * from a firmware with no network.
 *
 * The display is OPTIONAL AT RUNTIME. panelBegin() probes for a panel and
 * returns false if none answers; everything else then does nothing. A camera
 * with no screen attached behaves exactly as it did before, which is what
 * lets this ship before the hardware arrives.
 */
#ifdef WUW_PANEL

#include "panel.h"
#include <LovyanGFX.hpp>
#include <JPEGDEC.h>
#include <SPI.h>
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_heap_caps.h"
#include "SD_MMC.h"
#include <Preferences.h>
#include "esp_log.h"
#include "video_writer.h"
#include "ui_art.h"
#include "bwo_game.h"
#include "game_art.h"
#include "link.h"
#include "wuw_gates.h"
#ifdef EXHIBITION_MODE
#include "pasar.h"
#endif

/* Declared up here so the OS body can call them; defined with the bridge at
   the foot of the file. The panel never touches the camera itself -- it asks
   through the same serialised shutter the web handlers use. */
typedef bool (*PanelShootFn2)(const char*);
/* Our HR2046 reader, defined with the touch code further down.
   LovyanGFX's XPT2046 driver batches conversions under one CS assertion;
   this clone only returns a valid first conversion, so production never uses
   the library touch path. */
static bool panelTouchGet(int32_t* outX, int32_t* outY);
static bool touchCalValid();
static PanelShootFn fnShootFwd = nullptr;
static PanelRecFn fnRecFwd = nullptr;
static PanelCameraFn fnCameraFwd = nullptr;
static PanelCollectiveFn fnCollectiveFwd = nullptr;
static PanelWifiApi    wifiApi = {};          // set by panelWifiHooks()
static PanelLabelFn    fnLabelFwd = nullptr;  // names ISO / exposure stops

/* ── Panel pins, arranged to match the board's physical header ────────────
 * The camera owns GPIO 4-18 and the onboard microSD owns 38/39/40, both fixed
 * in hardware. Octal PSRAM takes 33-37, flash 26-32, UART0 43/44. What is left
 * is 0, 1, 2, 3, 14, 19, 20, 21, 41, 42, 45, 46, 47, 48 -- and 0, 3, 45 and 46
 * are strapping pins that a display module must not hold at the wrong level
 * during reset, so they are left alone.
 *
 * That leaves exactly seven usable pins for seven display signals, and they
 * fall into two runs that are CONTIGUOUS on the header:
 *
 *      1  MISO  (bridge B3)  ─┐
 *      2  MOSI  (bridge B2)   │  one ribbon, four adjacent pins
 *     42  T_CS                │
 *     41  SCK   (bridge B1)  ─┘
 *
 *     48  LED   (backlight) ─┐
 *     47  DC                  │  one ribbon, three adjacent pins
 *     21  CS                 ─┘
 *
 * The six pins between the runs are the microSD and the PSRAM, so a single
 * unbroken run is not available at any assignment -- two is the best there is.
 *
 * Ordering within each run follows the module's own header order. Bridge at
 * the module and take each single wire off the TOUCH pin of the pair (2, 3,
 * 5, which sit together near the top), and the seven wires leave the module
 * in the same order they arrive at the board: MISO, MOSI, T_CS, SCK, LED,
 * DC, CS. No ribbon crosses itself. Drawing it is what showed this.
 *
 * Two placements are deliberate rather than convenient:
 *
 *   GPIO 48 also feeds an onboard RGB LED on these boards, so whatever sits
 *   there carries an extra stub. It gets the BACKLIGHT -- a line that is
 *   either on or off and never toggles at speed. An earlier draft put CS
 *   here on the same reasoning, which was right in kind and wrong in degree:
 *   the backlight is slower still.
 *
 *   T_CS sits BETWEEN SCK and MOSI rather than beside them. A chip select
 *   idling high is the cheapest thing to put between a 40 MHz clock and its
 *   data line, and the run has room for it at no cost. */
#define LCD_MISO  1     // module SDO + T_DO bridged   (B3)
#define LCD_MOSI  2     // module SDI + T_DIN bridged  (B2)
#define LCD_SCLK 41     // module SCK + T_CLK bridged  (B1)
#define TCH_CS   42     // touch chip select
#define LCD_BL   48     // backlight; measure before driving it from a pin
#define LCD_DC   47     // data/command
#define LCD_CS   21     // display chip select
#define LCD_RST  20     // hardware reset; required for deterministic warm boots
/* T_IRQ is deliberately not connected. GPIO 14 belongs to the physical
   shutter button; pressure is read from Z1 over SPI. */
/* RESET is controlled explicitly. The third unit proved that software reset
   alone is insufficient after some upload and brownout sequences. */
/* ── SPI clock ────────────────────────────────────────────────────────────
 * MEASURED on this harness: 10 MHz renders the skin's gradients cleanly,
 * 20 MHz streaks them, 40 MHz is worse still.
 *
 * Why gradients specifically, when text and flat fills stayed crisp at every
 * speed: the skin ORDERED-DITHERS every gradient pixel, so neighbouring
 * pixels differ in their low bits by design. That is maximum toggling on the
 * data line -- the exact worst case for a marginal bus. A flat fill toggles
 * nothing and text is sparse, so both survive a clock the gradients cannot.
 * The dither is what made the wiring's limit visible; it is not the fault.
 *
 * This is a wiring number, not a code number. On breadboard jumpers 10 MHz is
 * where it lands; a soldered harness under 10 cm should take 40, and the
 * setting is exposed so that can be found by measuring rather than by
 * reflashing:  /panel?k=wuw01&hz=40
 */
#ifndef LCD_SPI_HZ
#define LCD_SPI_HZ 10000000
#endif

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
  /* ── Which controller ──────────────────────────────────────────────────
     Sold as an ILI9341, and it is not one. The controller sweep painted the
     same frame through five driver classes and only ST7789 rendered it
     upright, in the right colours, across the whole panel. The ID register
     never answers on this module, so the sweep was the only way to find out
     -- reading the silkscreen would have said ILI9341 and been wrong. */
  lgfx::Panel_ST7789  _panel;
  lgfx::Bus_SPI       _bus;
  /* ── Candidate controllers ─────────────────────────────────────────────
     The 2.4" red modules are sold as "ILI9341" and frequently are not. The
     ID register cannot settle it here because the read path is dead, so the
     alternatives are kept alive and swapped in: whichever one paints a
     correct, upright, correctly-coloured frame IS the controller.
     ILI9342 is the one to watch -- it is the 320x240-native sibling of the
     9341, and driving one as the other gives exactly a mirrored image over
     part of the panel. */
  lgfx::Panel_ILI9342 _p9342;
  lgfx::Panel_ILI9341 _p9341;
  lgfx::Panel_ST7796  _p7796;
public:
  LGFX() {
    { auto c = _bus.config();
      c.spi_host=SPI2_HOST; c.spi_mode=0;
      /* 40 MHz pushes a full frame in 34 ms, 80 MHz in 19 ms (measured).
         80 needs short, tidy wiring — on long dupont leads the panel shows
         torn or speckled tiles. Start at 40; raise once the wiring is neat. */
      /* Reads are NOT symmetrical with writes. The ILI9341 only drives SDO
         during a read and cannot hold the bus at display speed while doing
         it, so a register read above about 6 MHz returns garbage -- which is
         exactly why so many people conclude their panel has no readable ID.
         Writes stay at LCD_SPI_HZ; this only slows the probe. */
      c.freq_write=LCD_SPI_HZ; c.freq_read=6000000;
      c.spi_3wire=false; c.use_lock=true; c.dma_channel=SPI_DMA_CH_AUTO;
      c.pin_sclk=LCD_SCLK; c.pin_mosi=LCD_MOSI; c.pin_miso=LCD_MISO; c.pin_dc=LCD_DC;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c = _panel.config();
      c.pin_cs=LCD_CS; c.pin_rst=LCD_RST; c.pin_busy=-1;
      c.panel_width=240; c.panel_height=320;
      c.bus_shared=true;
      /* MEASURED, not assumed. The sweep painted R/G/B squares and this panel
         showed them in the right order only with rgb_order TRUE -- the
         default of false put red on the blue channel, which is why the very
         first fill came back "blueish". */
      /* BGR. The R/G/B strip came back in the right order only this way on
         the ST7789; the earlier "rgb_order=true" finding was read off an
         ILI9341 frame, which is a different chip and a different answer. */
      c.rgb_order=false;
      c.invert=false;          // black stayed black; this panel is not inverted
      /* Nothing ever answers the ID register on this module, so telling the
         driver it is readable only buys dead time waiting for a reply. */
      c.readable=false;
      _panel.config(c); }
    setPanel(&_panel);
  }

  /* RDID4 (0xD3) answers 00 93 41 on an ILI9341 and nothing coherent on an
     empty bus. Exposed here because _panel is private and this is the only
     thing outside the class that needs it. */
  uint32_t readPanelId() { return _panel.readCommand(0xD3, 0, 4); }

  /* Re-point the driver at a different set of pins and bring it up again.
     For the bench sweep: the WRITE path needs no MISO, so a map can be tested
     by painting with it even when nothing can be read back. Note pin_dc lives
     in the BUS config, not the panel config -- a detail that is easy to get
     wrong and produces exactly the symptom being chased. */
  /* Swap the driver class under the same bus and pins. rgbOrder is exposed
     because red arriving as blue is a one-bit fix, not a rewire. */
  bool usePanel(int which, bool rgbOrder, bool invert, int hz) {
    lgfx::Panel_Device* p;
    switch (which) {
      case 1:  p = &_p9342; break;
      case 2:  p = &_p9341; break;
      case 3:  p = &_p7796; break;
      default: p = &_panel; break;
    }
    auto bc = _bus.config();
    bc.freq_write = hz;
    _bus.config(bc);
    auto c = p->config();
    c.pin_cs = LCD_CS; c.pin_rst = LCD_RST; c.pin_busy = -1;
    c.bus_shared = true; c.readable = false;   // nothing answers; do not wait on it
    /* ST7789 panels are almost always inverted; ILI9341 almost never.
       Getting it wrong gives a photographic negative, not a failure. */
    c.invert = invert;
    c.rgb_order = rgbOrder;
    p->config(c);
    p->setBus(&_bus);
    setPanel(p);
    return init();
  }

  /* DMA on/off and clock, for the bench sweep. Bulk pixel pushes and solid
     fills take different paths inside the driver -- only the former goes
     through DMA -- so a fault that hits gradients and camera frames while
     leaving text crisp points at DMA, not at the bus. */
  bool setBusMode(int hz, bool dma) {
    auto c = _bus.config();
    c.freq_write = hz;
    c.dma_channel = dma ? SPI_DMA_CH_AUTO : 0;
    _bus.config(c);
    return init();
  }

  bool reconfigure(int sck, int mosi, int miso, int cs, int dc, int hz) {
    auto c = _bus.config();
    c.pin_sclk = sck; c.pin_mosi = mosi; c.pin_miso = miso; c.pin_dc = dc;
    c.freq_write = hz; c.freq_read = 6000000;
    _bus.config(c);
    auto p = _panel.config();
    p.pin_cs = cs;
    _panel.config(p);
    return init();
  }
};

static LGFX      lcd;
static JPEGDEC   jdec;
static bool      lcdOK=false, camOK=false;

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
static volatile int  capReady = -1;            // index holding a complete frame
static volatile uint32_t capSeq = 0, capUs = 0, capDecUs = 0;

static inline int luma565(uint16_t p) {
  int r=((p>>11)&0x1F)<<3, g=((p>>5)&0x3F)<<2, b=(p&0x1F)<<3;
  return (77*r + 150*g + 29*b) >> 8;
}
  // timing probe, writes nothing

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
    {0x06,0x07,0x0a}, {0x2a,0x31,0x3a}, {0x33,0x3b,0x45}, {0x17,0x1b,0x21},
    {0x7a,0x87,0x94}, {0x04,0x06,0x0a}, {0xc6,0xcd,0xd3}, {0x78,0x82,0x8c},
    {0x79,0xc2,0xa4}, {0xe8,0x78,0x8f}, 0, 0 },
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
  { "FAIRY",
    /* Angelic pearl: moonlit lilac metal, icy highlights, soft rose signal. */
    {0x07,0x07,0x10}, {0x35,0x32,0x46}, {0x75,0x6f,0x92}, {0x19,0x17,0x24},
    {0xd9,0xd5,0xec}, {0x03,0x03,0x09}, {0xfb,0xf8,0xff}, {0xb7,0xb2,0xc8},
    {0xcd,0xef,0xff}, {0xff,0x92,0xc7}, 32, 0 },
};
#define NSKIN ((int)(sizeof(SKINS)/sizeof(SKINS[0])))
static int uiSkin = 0;
#define SK (SKINS[uiSkin])

static inline uint16_t rgb(const uint8_t* c){ return lcd.color565(c[0],c[1],c[2]); }

static uint16_t rgbMix(const uint8_t* a, const uint8_t* b, uint8_t amount) {
  uint8_t c[3];
  for (int i = 0; i < 3; ++i)
    c[i] = (uint8_t)(((uint16_t)a[i] * (255 - amount) +
                      (uint16_t)b[i] * amount) / 255);
  return rgb(c);
}

/* Four broad tonal planes read as depth at 10 MHz without the command cost of
   a per-row gradient. Etching is sparse and skin-specific; no bitmap or work
   buffer is allocated for it. */
static void wuwPanelSkin(const Skin& skin, int x, int y, int w, int h,
                         bool pressed) {
  if (w <= 0 || h <= 0) return;
  if (h >= 10 && y + h < PH)
    lcd.fillRect(x + 2, y + h, max(0, w - 2), min(2, PH - y - h),
                 rgb(skin.shadow));

  const uint8_t* top = pressed ? skin.loBot : skin.hiTop;
  const uint8_t* mid = pressed ? skin.hiTop : skin.horizon;
  const uint8_t* bot = pressed ? skin.shadow : skin.loBot;
  int innerH = max(1, h - 2);
  int a = max(1, innerH * 30 / 100);
  int b = max(1, innerH * 22 / 100);
  int c = max(1, innerH * 25 / 100);
  int d = max(0, innerH - a - b - c);
  lcd.fillRect(x + 1, y + 1, max(0, w - 2), a, rgbMix(top, mid, 48));
  lcd.fillRect(x + 1, y + 1 + a, max(0, w - 2), b, rgbMix(top, mid, 150));
  lcd.fillRect(x + 1, y + 1 + a + b, max(0, w - 2), c, rgbMix(mid, bot, 115));
  if (d) lcd.fillRect(x + 1, y + 1 + a + b + c, max(0, w - 2), d,
                      rgbMix(mid, bot, 210));

  uint16_t etch = rgbMix(skin.loBot, skin.accent, 54);
  if (!pressed && h >= 18) {
    if (skin.etch == 1) {
      for (int yy = y + 5; yy < y + h - 2; yy += 6)
        lcd.drawFastHLine(x + 3, yy, max(0, w - 6), etch);
    } else if (skin.etch == 2) {
      for (int xx = x + 5; xx < x + w - 5; xx += 12)
        lcd.drawLine(xx, y + 2, min(xx + 5, x + w - 3), y + 7, etch);
    } else if (skin.etch == 3) {
      lcd.drawLine(x + 4, y + h / 2, x + w / 2, y + 3, etch);
      lcd.drawLine(x + w / 2, y + 3, x + w - 5, y + h / 2, etch);
    }
  }

  uint16_t lip = rgb(pressed ? skin.shadow : skin.lip);
  uint16_t sha = rgb(pressed ? skin.lip    : skin.shadow);
  lcd.drawFastHLine(x, y,         w, lip);
  lcd.drawFastVLine(x, y,         h, lip);
  lcd.drawFastHLine(x, y + h - 1, w, sha);
  lcd.drawFastVLine(x + w - 1, y, h, sha);
  if (!pressed && h >= 8 && skin.spec)
    lcd.drawFastHLine(x + 3, y + 2,
                      min(w - 6, max(4, (w - 6) * skin.spec / 64)),
                      rgbMix(skin.hiTop, skin.lip, 180));
}

static void wuwPanel(int x, int y, int w, int h, bool pressed) {
  wuwPanelSkin(SK, x, y, w, h, pressed);
}


/* Per-pixel looks, applied to the decoded RGB565 before it reaches the glass */
/* ── Gallery: reviewing what the camera shot, on the camera ───────────────
 * Files are MJPEG in a QuickTime wrapper, which turns out to be the useful
 * property here: every frame is an independent JPEG, so playback needs no
 * container parsing and no keyframe search -- scan for SOI/EOI and decode
 * whatever falls between. Seeking is likewise just a byte offset.
 */
#define SD_CLK_PIN 39
#define SD_CMD_PIN 38
#define SD_D0_PIN  40

#define GAL_MAX 160
#define GAL_PATH_MAX 64
static char     galName[GAL_MAX][GAL_PATH_MAX];
static uint8_t  galVid[GAL_MAX];
static uint16_t galNum[GAL_MAX];
static int      galN = 0, galSel = 0, galTop = 0;
static uint32_t galRevision = 0;
static bool     sdOk = false;
static bool     bwoWallsLoaded = false;
static char     cardNotice[28] = "";
static uint32_t cardNoticeUntil = 0;


/* contact sheet: 3 across, 2 down, inside the viewfinder window */
#define GCOLS 3
#define GROWS 2
#define GCW   (PW / GCOLS)
#define GCH   (VF_H / GROWS)

static uint16_t* viewBuf = nullptr;      // full-screen decode target

static uint16_t panelFxColor(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static uint8_t panelFxLuma(uint16_t p) {
  int r = ((p >> 11) & 31) * 255 / 31;
  int g = ((p >> 5) & 63) * 255 / 63;
  int b = (p & 31) * 255 / 31;
  return (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

static void applyPanelFx(uint16_t* pixels, int w, int h, int stride) {
  if (!pixels || uiFx == FX_NONE) return;
  static uint8_t prior[PW];
  if (uiFx == FX_EDGE) memset(prior, 0, sizeof(prior));
  for (int y = 0; y < h; ++y) {
    uint8_t left = 0;
    for (int x = 0; x < w; ++x) {
      uint16_t p = pixels[(size_t)y * stride + x];
      uint8_t l = panelFxLuma(p);
      uint16_t out = p;
      if (uiFx == FX_MONO) {
        out = panelFxColor(l, l, l);
      } else if (uiFx == FX_INVERT) {
        int r = 255 - ((p >> 11) & 31) * 255 / 31;
        int g = 255 - ((p >> 5) & 63) * 255 / 63;
        int b = 255 - (p & 31) * 255 / 31;
        out = panelFxColor(r, g, b);
      } else if (uiFx == FX_THERMAL) {
        int r = constrain((int)l * 3 - 180, 0, 255);
        int g = constrain(255 - abs((int)l - 128) * 2, 0, 255);
        int b = constrain(300 - (int)l * 2, 0, 255);
        out = panelFxColor(r, g, b);
      } else if (uiFx == FX_POSTER) {
        int r = (((p >> 11) & 31) * 255 / 31) & 0xC0;
        int g = (((p >> 5) & 63) * 255 / 63) & 0xC0;
        int b = ((p & 31) * 255 / 31) & 0xC0;
        out = panelFxColor(r, g, b);
      } else if (uiFx == FX_EDGE) {
        int d = abs((int)l - left) + abs((int)l - prior[x]);
        int v = constrain(d * 3, 0, 255);
        out = panelFxColor(v / 4, v, v);
        prior[x] = l;
        left = l;
      }
      pixels[(size_t)y * stride + x] = out;
    }
  }
}

/* Ghost owns the previous-frame buffer when active. On the home screen an
   opt-in exact comparison reuses it to skip unchanged display strips. */
static uint16_t* prevBuf = nullptr;
static bool tileOn = false;
static uint32_t stTiles = 0, stTotal = 0, stUs = 0, stFrames = 0;
static uint32_t viewWindowAt = 0, viewWindowFrames = 0, viewWindowUs = 0;
static float viewFps = 0, viewPushMs = 0;

static uint8_t*  fileBuf = nullptr;      // one JPEG, read off the card
#define FILEBUF_CAP (320 * 1024)

/* Generic decode target, so thumbnails and full frames share one callback.
   jdStride is the allocated destination width; jdW is only the visible image
   width. Keeping those separate matters whenever the power-of-two JPEG scale
   produces (for example) a 200 px image in the panel's 320 px work buffer. */
static uint16_t* jdBuf = nullptr;
static int jdW = 0, jdH = 0, jdStride = 0;
static int jdDrawG(JPEGDRAW* p) {
  if (!jdBuf) return 0;
  for (int r = 0; r < p->iHeight; r++) {
    int py = p->y + r;
    if (py < 0 || py >= jdH) continue;
    int n = p->iWidth;
    if (p->x + n > jdW) n = jdW - p->x;
    if (n > 0) memcpy(jdBuf + (size_t)py*jdStride + p->x,
                      p->pPixels + (size_t)r*p->iWidth, (size_t)n*2);
  }
  return 1;
}

/* JPEGDEC can stream from an Arduino File and deliver one MCU-sized RGB565
   block at a time. This path is what makes the on-camera gallery usable on
   boards whose PSRAM cannot be initialized: no whole JPEG and no 320x240
   framebuffer need to exist in RAM. */
static int jdDrawLCD(JPEGDRAW* p) {
  if (!p || !p->pPixels || p->iWidth <= 0 || p->iHeight <= 0) return 0;
  lcd.pushImage(p->x, p->y, p->iWidth, p->iHeight, p->pPixels);
  return 1;
}

static int32_t jdFileRead(JPEGFILE* jf, uint8_t* out, int32_t len) {
  return static_cast<File*>(jf->fHandle)->read(out, len);
}

static int32_t jdFileSeek(JPEGFILE* jf, int32_t pos) {
  return static_cast<File*>(jf->fHandle)->seek(pos) ? 1 : 0;
}

static bool decodeFileToLCD(const char* name) {
  String path = (name && name[0] == '/') ? String(name) : String("/") + name;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.isDirectory()) { if (f) f.close(); return false; }
  if (!jdec.open(&f, (int)f.size(), nullptr, jdFileRead, jdFileSeek, jdDrawLCD)) {
    f.close();
    return false;
  }

  int w = jdec.getWidth(), h = jdec.getHeight();
  int sc = 0, div = 1;
  if      (w > PW * 4 || h > PH * 4) { sc = JPEG_SCALE_EIGHTH; div = 8; }
  else if (w > PW * 2 || h > PH * 2) { sc = JPEG_SCALE_QUARTER; div = 4; }
  else if (w > PW     || h > PH)     { sc = JPEG_SCALE_HALF; div = 2; }
  int dw = w / div, dh = h / div;
  lcd.fillScreen(rgb(SK.bg));
  lcd.startWrite();
  int ok = jdec.decode((PW - dw) / 2, (PH - dh) / 2, sc);
  lcd.endWrite();
  jdec.close();
  f.close();
  return ok != 0;
}

static bool galInteresting(const char* n, bool* video) {
  size_t L = n ? strlen(n) : 0;
  bool jpg = L > 4 && !strcasecmp(n + L - 4, ".jpg");
  bool mov = L > 4 && (!strcasecmp(n + L - 4, ".mov") ||
                       !strcasecmp(n + L - 4, ".mp4"));
  if (video) *video = mov;
  return (jpg && !strstr(n, "_view.jpg")) || mov;
}

static void galScanDir(const char* path) {
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  File f;
  while (galN < GAL_MAX && (f = dir.openNextFile())) {
    if (!f.isDirectory()) {
      const char* n = f.name();
      while (*n == '/') n++;
      char full[GAL_PATH_MAX];
      if (strcmp(path, "/") && !strchr(n, '/')) {
        const char* d = path;
        while (*d == '/') d++;
        snprintf(full, sizeof(full), "%s/%s", d, n);
        n = full;
      }
      bool video = false;
      if (strlen(n) < GAL_PATH_MAX && galInteresting(n, &video)) {
        snprintf(galName[galN], sizeof(galName[0]), "%s", n);
        galVid[galN] = video ? 1 : 0;
        const char* u = strrchr(n, '_');
        galNum[galN] = u ? (uint16_t)atoi(u + 1) : 0;
        galN++;
      }
    }
    f.close();
  }
  dir.close();
}

static void galScan() {
  ++galRevision;
  galN = 0;
  galScanDir("/");
#ifdef EXHIBITION_MODE
  /* Stills are written inside the active PASAR session, not at SD root. */
  galScanDir(pasarSessionDir());
#endif
  // newest first -- insertion sort, descending by file number
  for (int i = 1; i < galN; i++)
    for (int j = i; j > 0 && galNum[j] > galNum[j-1]; j--) {
      char t[GAL_PATH_MAX]; memcpy(t, galName[j], GAL_PATH_MAX);
      memcpy(galName[j], galName[j-1], GAL_PATH_MAX);
      memcpy(galName[j-1], t, GAL_PATH_MAX);
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
  jdStride = dw;
  memset(dst, 0, (size_t)dw * dh * 2);
  jdec.setPixelType(RGB565_LITTLE_ENDIAN);
  jdec.decode(0, 0, sc);
  jdec.close();
  if (outW) *outW = jdW;
  if (outH) *outH = jdH;
  return true;
}

/* Stream a full-size SD JPEG through JPEGDEC into the existing panel buffer.
   This avoids the 320 KB fileBuf ceiling: a high-quality 5 MP still can be
   much larger, but the wall atlas needs only the decoded panel-size result. */
static bool decodeFileInto(const char* name, uint16_t* dst, int dw, int dh,
                           int* outW, int* outH) {
  if (!name || !*name || !dst) return false;
  String path = name[0] == '/' ? String(name) : String("/") + name;
  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.isDirectory()) { if (f) f.close(); return false; }
  if (!jdec.open(&f, (int)f.size(), nullptr, jdFileRead, jdFileSeek, jdDrawG)) {
    f.close();
    return false;
  }
  int w = jdec.getWidth(), h = jdec.getHeight();
  int sc = 0;
  if      (w > dw*4 || h > dh*4) sc = JPEG_SCALE_EIGHTH;
  else if (w > dw*2 || h > dh*2) sc = JPEG_SCALE_QUARTER;
  else if (w > dw   || h > dh)   sc = JPEG_SCALE_HALF;
  int div = sc == JPEG_SCALE_EIGHTH ? 8 : sc == JPEG_SCALE_QUARTER ? 4
          : sc == JPEG_SCALE_HALF ? 2 : 1;
  jdW = min(dw, w / div); jdH = min(dh, h / div);
  jdBuf = dst; jdStride = dw;
  memset(dst, 0, (size_t)dw * dh * sizeof(uint16_t));
  jdec.setPixelType(RGB565_LITTLE_ENDIAN);
  int ok = jdec.decode(0, 0, sc);
  jdec.close();
  f.close();
  if (outW) *outW = jdW;
  if (outH) *outH = jdH;
  return ok != 0;
}

static bool bwoPatchPalette = false;
/* Slot the most recent gallery photo was written to. bwoLoadWalls() re-adds
   every texture in order, and each add selects the slot it just wrote, so after
   a reload the LAST slot is selected -- not the new photo. With a full atlas the
   new photo replaces an earlier slot, so you would go on to place the OLD photo
   and conclude the new one had not worked. */
static int bwoLastWallSlot = -1;
static void bwoLoadWalls();
static bool bwoRememberWall(const char* name) {
  if (!name || !*name || strlen(name) >= GAL_PATH_MAX || strstr(name, ".."))
    return false;
  const char* clean = name[0] == '/' ? name + 1 : name;
  size_t len = strlen(clean);
  if (len < 5 || strcasecmp(clean + len - 4, ".jpg")) return false;
  if (bwoPatchPalette) {
    int w=0,h=0;
    bool ok = sdOk && viewBuf && decodeFileInto(name,viewBuf,PW,PH,&w,&h) &&
      bwoGameAddWallTexture(viewBuf,w,h,PW);
    Serial.printf("[BWO] wall photo into loaded world's palette: %s -> %s\n",
                  clean, ok ? "ok" : "FAILED");
    return ok;
  }
  int decodedW=0, decodedH=0;
  if (!sdOk || !viewBuf ||
      !decodeFileInto(name,viewBuf,PW,PH,&decodedW,&decodedH)) {
    Serial.printf("[BWO] wall photo REJECTED (%s): %s\n",
                  !sdOk ? "no SD card" : !viewBuf ? "no frame buffer" :
                  "JPEG would not decode", clean);
    return false;
  }

  String paths[BWO_WALL_TEXTURE_SLOTS];
  Preferences bp;
  bp.begin("bwowalls", false);
  int count = min((int)bp.getUChar("n", 0), BWO_WALL_TEXTURE_SLOTS);
  for (int i = 0; i < count; ++i) {
    char key[4]; snprintf(key, sizeof(key), "p%d", i);
    paths[i] = bp.getString(key, "");
  }
  for (int i = 0; i < count; ++i) {
    if (paths[i] == clean) {
      bp.end();
      bwoLoadWalls();
      bwoLastWallSlot = i;
      Serial.printf("[BWO] wall photo already in slot %d: %s\n", i + 1, clean);
      return bwoGameSelectWallTexture(i);
    }
  }
  // Slot IDs are permanent world references. Replacing a full atlas changes
  // only one material, never shifts all existing cube-face assignments.
  int slot = count < BWO_WALL_TEXTURE_SLOTS ? count++ :
             bp.getUChar("replace", 0) % BWO_WALL_TEXTURE_SLOTS;
  paths[slot] = clean;
  bwoLastWallSlot = slot;
  Serial.printf("[BWO] wall photo -> slot %d of %d: %s (%dx%d)\n", slot + 1,
                BWO_WALL_TEXTURE_SLOTS, clean, decodedW, decodedH);
  if (count == BWO_WALL_TEXTURE_SLOTS)
    bp.putUChar("replace", (slot + 1) % BWO_WALL_TEXTURE_SLOTS);
  bp.putUChar("n", count);
  for (int i = 0; i < BWO_WALL_TEXTURE_SLOTS; ++i) {
    char key[4]; snprintf(key, sizeof(key), "p%d", i);
    if (i < count) bp.putString(key, paths[i]);
    else           bp.remove(key);
  }
  bp.end();
  bwoWallsLoaded = false;
  return true;
}

static void bwoLoadWalls() {
  if (bwoWallsLoaded) return;
  if (!sdOk || !viewBuf) return;
  bwoWallsLoaded = true;
  bwoGameClearWallTextures();
  Preferences bp;
  bp.begin("bwowalls", true);
  int count = min((int)bp.getUChar("n", 0), BWO_WALL_TEXTURE_SLOTS);
  for (int i = 0; i < count; ++i) {
    char key[4]; snprintf(key, sizeof(key), "p%d", i);
    String path = bp.getString(key, "");
    int w = 0, h = 0;
    if (path.length() && decodeFileInto(path.c_str(), viewBuf, PW, PH, &w, &h))
      bwoGameAddWallTexture(viewBuf, w, h, PW);
    else {
      // A magenta wall is the visible symptom; the log says which file it was.
      Serial.printf("[BWO] wall slot %d: cannot decode '%s' -> magenta placeholder\n",
                    i + 1, path.c_str());
      static const uint16_t missing = 0xf81f;
      bwoGameAddWallTexture(&missing, 1, 1, 1);
    }
  }
  bp.end();
  Serial.printf("[BWO] %u gallery wall textures loaded\n",
                (unsigned)bwoGameWallTextureCount());
}

static void galShowPhoto(int idx) {
  if (idx < 0 || idx >= galN) return;
  if (!decodeFileToLCD(galName[idx])) return;
  lcd.setTextDatum(bottom_left);
  lcd.setTextColor(rgb(SK.shadow)); lcd.drawString(galName[idx], 8, PH - 6);
  lcd.setTextColor(rgb(SK.text));   lcd.drawString(galName[idx], 7, PH - 7);
  lcd.setTextDatum(top_left);

  lcd.setTextDatum(bottom_right);
  lcd.setTextColor(rgb(SK.text));
  lcd.drawString("TAP TO CLOSE", PW - 7, PH - 7);
  lcd.setTextDatum(top_left);
  uint32_t until = millis() + 30000;
  while ((int32_t)(until - millis()) > 0) {
    int32_t tx, ty;
    if (panelTouchGet(&tx, &ty)) {
      while (panelTouchGet(&tx, &ty)) vTaskDelay(pdMS_TO_TICKS(15));
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
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
          if (panelTouchGet(&tzx, &tzy)) { f.close();   // tap to stop
            Serial.printf("[GAL] play stopped after %lu frames\n",
                          (unsigned long)frames);
            return; }
        }
      }
      prev = c;
    }
  }
  f.close();
  uint32_t ms = millis() - t0;
  Serial.printf("[GAL] played %lu frames in %lu ms (%.1f fps)\n",
                (unsigned long)frames, (unsigned long)ms,
                ms ? frames * 1000.0f / ms : 0.0f);
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

/* The Nacre art decodes to 320x180. The OS body is 320x168, so six rows are
   cropped from each edge. These coordinates are the clear glass aperture in
   that cropped frame, inset slightly so camera pixels never cover its rim. */
#define HOME_VIEW_X 31
#define HOME_VIEW_Y 31
#define HOME_VIEW_W 258
#define HOME_VIEW_H 141

enum { S_HOME = 0, S_LOOKS, S_TUNE, S_CARD, S_MORE,
       S_PRESET, S_ROOM, S_SET, S_CAMERA, S_SYS, S_WIFI,
       S_GAMES, S_COLLECTIVE, S_GHOST, S_APOPHENIA, S_ORACLE,
       S_GAME, S_RELAY, S_EXPOSURE, S_GATES,
       S_BWO_PATCHES, S_BWO_STYLE, S_BWO_MANAGE, S_BWO_PHOTOS, S_LIGHT, S_COUNT };
static uint8_t bwoPatchSelection = 0;
static bool bwoPatchSaved[4] = {};
static bool bwoPatchScanned = false;
static uint32_t bwoResetUntil = 0, bwoSaveUntil = 0;
static const char* SCREEN_NAME[S_COUNT] =
  { "VIEW","LOOKS","TUNE","CARD","MORE","PRESETS","ROOM","SKINS","CAMERA","SYSTEM","WIFI",
    "RITUAL GAMES","COLLECTIVE","GHOST RESIDUE","APOPHENIA","ORACLE",
    "BODY WITHOUT ORGANS","RITUAL RELAY","DIVINATION","EIGHT GATES",
    "BODY WITHOUT ORGANS","WORLD STYLE","WORLD TOOLS","WALL PHOTOS","LIGHT" };

static int  osScreen = S_HOME;
static int  osCardReturn = S_HOME;
static int  osPage = 0, osSel = 0;
static int8_t osHeld = -1;
/* Touch and repaint state, declared with the rest of the OS state because the
   keyboard and WiFi screens (which sit above the touch code) need to read and
   set it. */
static uint8_t  osIso = 0, osExp = 0;    // LIGHT: 0 = AUTO for both
static const char* const ISO_LABEL[PANEL_ISO_STOPS] =      // fallback; the camera names its own
  { "AUTO", "100", "200", "400", "800", "1600", "3000" };
/* The camera side names each stop, because only it knows what the sensor can
   do (the top ISO stop is 30x, so it is "3000", and shutter fractions come from
   a measured frame period). Falls back to plain labels if it cannot. */
static void lightName(bool iso, uint8_t idx, char* out, size_t n) {
  const char* t = fnLabelFwd ? fnLabelFwd(iso ? PANEL_CAM_ISO : PANEL_CAM_EXPOSURE, idx) : nullptr;
  if (t && *t)  snprintf(out, n, "%s", t);
  else if (iso) snprintf(out, n, "%s", ISO_LABEL[idx % PANEL_ISO_STOPS]);
  else if (!idx) snprintf(out, n, "AUTO");
  else          snprintf(out, n, "M%u", (unsigned)idx);
}
static bool     osDown = false;          // a press is in progress
static bool     osHomeTouch = false;     // that press began inside the viewfinder
static uint32_t osRepaint = 1;           // set whenever the screen must redraw
static void osGo(int screen);
static PanelStatus pstat = {};
static uint8_t osViewRot = 1;       // panel preview only: 0, 1, 2, 3 clockwise turns
static uint16_t osZoomPct = 100;   // resistive screen uses horizontal drag
static uint8_t gateMask = 0;
static bool gateLoaded = false;
static bool gateMetricsValid = false;
static WuwGateMetrics gateMetrics = {};
static char gateNotice[32] = "";
static uint32_t gateNoticeUntil = 0;

/* ── HUDs that sit on live video ─────────────────────────────────────────────
 * Every camera frame used to be pushed over the whole 320x168 body and then
 * the HUD panels, emblem and text were painted back on top of it: about 85 ms
 * of wiping followed by a repaint, twelve times a second. The glass showed the
 * HUD missing and then arriving, over and over.
 *
 * Now the push SKIPS the HUD rectangles (the panels are opaque, so the picture
 * is identical) and the HUD is redrawn only when what it says has changed, or
 * after a real repaint of the screen. Between those it simply stays on the
 * glass, untouched. */
static bool hudForce = true;          // set by osDraw(): the glass was repainted
static uint32_t hudHash(uint32_t h, const char* s) {
  for (; s && *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
  return (h ^ 0xFFu) * 16777619u;
}
static uint32_t hudHashInt(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static bool hudNoticeActive() {
  return gateNotice[0] && (int32_t)(gateNoticeUntil - millis()) > 0;
}
static uint32_t gateResetArmedUntil = 0;
static void osChooseSkin(int index);

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
/* Text on ANY surface -- the glass or an off-screen sprite. Overlays that sit
   on top of moving pictures are drawn into memory and composited into the frame
   before it is pushed, so every pixel reaches the glass once, already final.
   Drawing them on the glass after the frame is what makes text flicker. */
static void osTextOn(lgfx::LGFXBase& g, const char* s, int x, int y, uint16_t col,
                     uint8_t datum, bool shadow) {
  g.setTextDatum((textdatum_t)datum);
  if (shadow) { g.setTextColor(rgb565(SK.shadow)); g.drawString(s, x, y + 1); }
  g.setTextColor(col);
  g.drawString(s, x, y);
  g.setTextDatum(top_left);
}
static void osText(const char* s, int x, int y, uint16_t col,
                   uint8_t datum = textdatum_t::top_left, bool shadow = true) {
  osTextOn(lcd, s, x, y, col, datum, shadow);
}

static void osCyberSplash() {
  if (!viewBuf) return;
  int w = 0, h = 0;
  size_t artLen = 0;
  const uint8_t* art = wuwUiArtForSkin(SK.name, &artLen);
  if (!decodeInto(const_cast<uint8_t*>(art), artLen,
                  viewBuf, PW, PH, &w, &h)) return;
  lcd.fillScreen(rgb565(SK.bg));
  int ox = (PW - w) / 2, oy = (PH - h) / 2;
  for (int r = 0; r < h; ++r)
    lcd.pushImage(ox, oy + r, w, 1, viewBuf + (size_t)r * PW);
  lcd.fillRect(62, 87, 196, 63, lcd.color565(3, 7, 8));
  lcd.drawRect(62, 87, 196, 63, rgb565(SK.accent));
  osText("WUW CAM", PW / 2, 99, rgb565(SK.text), textdatum_t::top_center);
  osText("CYBER OPTICAL UNIT", PW / 2, 119, rgb565(SK.accent), textdatum_t::top_center);
#ifdef WUW_SENSOR_OV3660
  osText("S3 / OV3660", PW / 2, 135, inkOn(), textdatum_t::top_center);
#else
  osText("S3 / OV5640", PW / 2, 135, inkOn(), textdatum_t::top_center);
#endif
  delay(650);
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
  } else if (!strcmp(k, "eye")) {
    lcd.drawEllipse(X(8), Y(8), (int)(7 * u), (int)(4 * u), col);
    lcd.fillCircle(X(8), Y(8), (int)(2 * u), col);
  } else if (!strcmp(k, "sun")) {
    lcd.drawCircle(X(8), Y(8), (int)(3 * u), col);
    for (int i = 0; i < 8; i++) { float a = i * 0.7854f;
      lcd.drawLine(X(8 + cosf(a) * 5), Y(8 + sinf(a) * 5),
                   X(8 + cosf(a) * 7.5f), Y(8 + sinf(a) * 7.5f), col); }
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
  static const BarKey card[5]  = {{"back","BACK"},{"card","OPEN"},{"mask","WALL"},
                                  {"grid","PAGE"},{"more","MORE"}};
  static const BarKey more[5]  = {{"aperture","VIEW"},{"",""},{"",""},
                                  {"",""},{"",""}};
  static const BarKey games[5] = {{"back","EXIT"},{"",""},{"",""},
                                  {"",""},{"more","MORE"}};
  static const BarKey game[5]  = {{"back","WORLDS"},{"sliders","STYLE"},{"aperture","FIRE"},
                                  {"box","PAUSE"},{"card","PHOTOS"}};
  static const BarKey patches[5] = {{"back","EXIT"},{"save","SAVE"},{"box","OPEN"},
                                    {"grid","STYLE"},{"more","TOOLS"}};
  static const BarKey style[5] = {{"back","BACK"},{"",""},{"box","PLAY"},
                                  {"card","PHOTOS"},{"more","WORLDS"}};
  static const BarKey manage[5] = {{"back","BACK"},{"",""},{"",""},{"",""},{"","CONFIRM"}};
  static const BarKey photos[5] = {{"back","BACK"},{"card","1"},{"card","2"},
                                   {"card","3"},{"card","4"}};
  static const BarKey gates[5] = {{"back","EXIT"},{"sliders","RESET"},{"aperture","SCAN"},
                                  {"mask","BWO"},{"more","MORE"}};
  static const BarKey collective[5] = {{"back","EXIT"},{"sliders","RESET"},
    {"aperture","SYNC"},{"people","PEERS"},{"more","MORE"}};
  static const BarKey ghost[5] = {{"back","EXIT"},{"sliders","RESET"},
    {"aperture","COLLECT"},{"card","SAVE"},{"more","MORE"}};
  static const BarKey apo[5] = {{"back","EXIT"},{"mask","I"},{"mask","II"},
    {"mask","III"},{"save","SAVE"}};
  static const BarKey oracle[5] = {{"back","EXIT"},{"grid","NEXT"},
    {"aperture","ANSWER"},{"save","SAVE"},{"more","MORE"}};
  static const BarKey relay[5] = {{"back","EXIT"},{"card","INHERIT"},
    {"aperture","SHOOT"},{"grid","EFFECT"},{"more","MORE"}};
  static const BarKey exposure[5] = {{"back","EXIT"},{"sliders","EXP-"},
    {"aperture","REVEAL"},{"sliders","EXP+"},{"more","MORE"}};
  static const BarKey wifi[5]  = {{"back","BACK"},{"wifi","SCAN"},{"grid","PAGE"},
                                  {"box","OTHER"},{"save","FORGET"}};
  static const BarKey light[5] = {{"back","BACK"},{"sun","ISO -"},{"sun","ISO +"},
                                  {"sliders","EXP -"},{"sliders","EXP +"}};
  static const BarKey leaf[5]  = {{"aperture","VIEW"},{"",""},{"aperture","SHOOT"},
                                  {"",""},{"more","MORE"}};
  const BarKey* src = leaf;
  switch (osScreen) {
    case S_HOME:  src = home;  break;
    case S_LOOKS: src = looks; break;
    case S_TUNE:  src = tune;  break;
    case S_CARD:  src = card;  break;
    case S_MORE:  src = more;  break;
    case S_GAMES: src = games; break;
    case S_GAME:  src = game;  break;
    case S_BWO_PATCHES: src = patches; break;
    case S_BWO_STYLE: src = style; break;
    case S_BWO_MANAGE: src = manage; break;
    case S_BWO_PHOTOS: src = photos; break;
    case S_GATES: src = gates; break;
    case S_COLLECTIVE: src = collective; break;
    case S_GHOST:      src = ghost; break;
    case S_APOPHENIA: src = apo; break;
    case S_ORACLE:     src = oracle; break;
    case S_RELAY:      src = relay; break;
    case S_EXPOSURE:   src = exposure; break;
    case S_WIFI:       src = wifi; break;
    case S_LIGHT:      src = light; break;
    default: break;
  }
  for (int i = 0; i < 5; i++) out[i] = src[i];
  if (osScreen == S_BWO_PATCHES &&
      !bwoPatchSaved[bwoPatchSelection]) out[2].label = "PLAY";
}
static void osBar() {
  BarKey k[5]; osBarKeys(k);
  for (int i = 0; i < NBTN; i++)
    wuwPanel(i * BTN_W, OS_BAR_Y, BTN_W - 1, OS_BAR_H - 1, i == osHeld);
  for (int i = 0; i < NBTN; i++) {
    const char* label = k[i].label;
    if (osScreen == S_GAME) {
      if      (i == 1 && bwoGameBuildMode()) label = "REMOVE";
      if      (i == 2 && bwoGameBuildMode()) label = "PLACE";
      else if (i == 3) label = bwoGameBuildMode() ? "PLAY" : "BUILD";
      else if (i == 4 && bwoGameBuildMode()) label = "PHOTOS";
    } else if (osScreen == S_CARD && i == 0 && osCardReturn == S_GAME) {
      label = "BWO";
    } else if (osScreen == S_BWO_MANAGE && i == 4 &&
               (int32_t)(bwoResetUntil-millis()) <= 0) {
      label = "";
    }
    if (!label[0]) continue;
    int cx = i * BTN_W + (BTN_W - 1) / 2;
    uint16_t col = ((i == 0 && osScreen == S_HOME) || !strcmp(label, "VIEW"))
                                                    ? rgb565(SK.accent)
                 : (osScreen == S_HOME && i == 1 && uiRec) ? rgb565(SK.rec)
                 : (!strcmp(label, "BACK") || !strcmp(label, "EXIT")) ? inkOn()
                 : rgb565(SK.text);
    if (k[i].icon[0]) osIcon(k[i].icon, cx - 8, OS_BAR_Y + 9, col, 16);
    osText(osScreen == S_HOME && i == 1 && uiRec ? "STOP" : label, cx, OS_BAR_Y + 30, col,
           textdatum_t::top_center);
  }
}

/* ── screens ─────────────────────────────────────────────────────────────── */
static void osGround(int y0, int y1, float t) {
  uint8_t c[3];
  for (int i = 0; i < 3; i++) c[i] = (uint8_t)(SK.bg[i] + (SK.shadow[i] - SK.bg[i]) * t);
  lcd.fillRect(0, y0, PW, y1 - y0, rgb(c));
}

#define HOME_LBL_H  11
#define HOME_LBL_MAXW 128
struct HomeStrip { int16_t x, w; };
static const HomeStrip HOME_STRIPS[3] = { {0, 84}, {96, 128}, {236, 84} };   // fx name | ISO/EXP | card
static uint16_t homeLblBg[3][HOME_LBL_MAXW * HOME_LBL_H];
static bool     homeLblBgOk = false;
static void homeLabelCapture(const uint16_t* art, int artW, int artH, int srcY);
static void osHomeFrame() {
  int w = 0, h = 0;
  size_t artLen = 0;
  const uint8_t* art = wuwUiArtForSkin(SK.name, &artLen);
  if (!viewBuf || !decodeInto(const_cast<uint8_t*>(art), artLen,
                  viewBuf, PW, PH, &w, &h)) {
    homeLblBgOk = false;      // flat ground: restoring means filling, not pasting
    osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.35f);
    return;
  }

  int srcY = (h > OS_BODY_H) ? (h - OS_BODY_H) / 2 : 0;
  int rows = h - srcY;
  if (rows > OS_BODY_H) rows = OS_BODY_H;
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.35f);
  for (int r = 0; r < rows; ++r)
    lcd.pushImage((PW - w) / 2, OS_BODY_Y + r, w, 1,
                  viewBuf + (size_t)(srcY + r) * PW);
  homeLabelCapture(viewBuf, w, h, srcY);
  lcd.fillRect(HOME_VIEW_X, HOME_VIEW_Y, HOME_VIEW_W, HOME_VIEW_H,
               lcd.color565(2, 5, 7));
}

/* Aspect-fill a decoded frame into the artwork's aperture. The crop is
   centered and uniform: a 4:3 sensor loses top/bottom content rather than
   being stretched into the border's wider glass. Nearest-neighbour sampling
   is deliberate at 258x141; it keeps this pass small beside JPEG decode and
   the 10 MHz wire transfer. */
static uint16_t homeStrip[HOME_VIEW_W * 8];
static uint16_t homeClip[HOME_VIEW_W * 8];
static void pushHomeSegment(int base, int firstRow, int rows, int firstCol, int width) {
  if (width <= 0 || rows <= 0) return;
  for (int row = 0; row < rows; ++row)
    memcpy(homeClip + (size_t)row * width,
           homeStrip + (size_t)(firstRow + row) * HOME_VIEW_W + firstCol,
           (size_t)width * sizeof(uint16_t));
  lcd.pushImage(HOME_VIEW_X + firstCol, HOME_VIEW_Y + base + firstRow,
                width, rows, homeClip);
}

static void pushHomeCamera(int srcW, int srcH) {
  if (!viewBuf || srcW <= 0 || srcH <= 0) return;
  uint32_t frameStarted = micros();

  int logicalW = (osViewRot & 1) ? srcH : srcW;
  int logicalH = (osViewRot & 1) ? srcW : srcH;
  int cropX = 0, cropY = 0, cropW = logicalW, cropH = logicalH;
  if ((int64_t)logicalW * HOME_VIEW_H > (int64_t)HOME_VIEW_W * logicalH) {
    cropW = (int)((int64_t)logicalH * HOME_VIEW_W / HOME_VIEW_H);
    cropX = (logicalW - cropW) / 2;
  } else {
    cropH = (int)((int64_t)logicalW * HOME_VIEW_H / HOME_VIEW_W);
    cropY = (logicalH - cropH) / 2;
  }
  if (cropW < 1) cropW = 1;
  if (cropH < 1) cropH = 1;

  cropW = max(1, cropW * 100 / osZoomPct);
  cropH = max(1, cropH * 100 / osZoomPct);
  cropX = (logicalW - cropW) / 2;
  cropY = (logicalH - cropH) / 2;
  static uint16_t xMap[HOME_VIEW_W], yMap[HOME_VIEW_H];
  static int lastW = -1, lastH = -1, lastRot = -1, lastZoom = -1;
  if (lastW != srcW || lastH != srcH || lastRot != (osViewRot & 3) ||
      lastZoom != osZoomPct) {
    stFrames = 0;
    for (int x = 0; x < HOME_VIEW_W; ++x)
      xMap[x] = cropX + x * cropW / HOME_VIEW_W;
    for (int y = 0; y < HOME_VIEW_H; ++y)
      yMap[y] = cropY + y * cropH / HOME_VIEW_H;
    lastW = srcW; lastH = srcH; lastRot = osViewRot & 3;
    lastZoom = osZoomPct;
  }

  uint32_t pushStarted = tileOn ? micros() : 0;
  int pushed = 0;
  lcd.startWrite();
  for (int base = 0; base < HOME_VIEW_H; base += 8) {
    int rows = min(8, HOME_VIEW_H - base);
    for (int ry = 0; ry < rows; ++ry) {
      int ly = yMap[base + ry];
      uint16_t* row = homeStrip + ry * HOME_VIEW_W;
      for (int dx = 0; dx < HOME_VIEW_W; ++dx) {
        int lx = xMap[dx], sx, sy;
        switch (osViewRot & 3) {
          case 1:  sx = ly;            sy = srcH - 1 - lx; break;
          case 2:  sx = srcW - 1 - lx; sy = srcH - 1 - ly; break;
          case 3:  sx = srcW - 1 - ly; sy = lx;            break;
          default: sx = lx;            sy = ly;             break;
        }
        sx = max(0, min(srcW - 1, sx));
        sy = max(0, min(srcH - 1, sy));
        row[dx] = viewBuf[(size_t)sy * PW + sx];
      }
    }
    size_t bytes = (size_t)HOME_VIEW_W * rows * 2;
    uint16_t* old = prevBuf ? prevBuf + (size_t)base * HOME_VIEW_W : nullptr;
    bool changed = !tileOn || !prevBuf || !stFrames ||
                   memcmp(homeStrip, old, bytes) != 0;
    if (changed) {
      // Leave the gallery key untouched. Painting a JPEG over a control and
      // drawing it again produces visible flicker even with a fast SPI clock.
      constexpr int iconX = HOME_VIEW_W - 44;
      constexpr int iconY = HOME_VIEW_H - 44;
      for (int first = 0; first < rows;) {
        bool blocked = base + first >= iconY && base + first < iconY + 42;
        int count = 1;
        while (first + count < rows &&
               (base + first + count >= iconY &&
                base + first + count < iconY + 42) == blocked) ++count;
        if (blocked) {
          pushHomeSegment(base, first, count, 0, iconX);
          pushHomeSegment(base, first, count, iconX + 42,
                          HOME_VIEW_W - iconX - 42);
        } else {
          lcd.pushImage(HOME_VIEW_X, HOME_VIEW_Y + base + first,
                        HOME_VIEW_W, count,
                        homeStrip + (size_t)first * HOME_VIEW_W);
        }
        first += count;
      }
      ++pushed;
    }
    if (tileOn && prevBuf) memcpy(old, homeStrip, bytes);
  }
  lcd.endWrite();
  if (tileOn) {
    stTiles += pushed;
    stTotal += (HOME_VIEW_H + 7) / 8;
    stUs += micros() - pushStarted;
    ++stFrames;
  }
  uint32_t now = millis();
  if (!viewWindowAt) viewWindowAt = now;
  ++viewWindowFrames;
  viewWindowUs += micros() - frameStarted;
  if (now - viewWindowAt >= 1000) {
    viewFps = viewWindowFrames * 1000.0f / (now - viewWindowAt);
    viewPushMs = viewWindowUs / (1000.0f * viewWindowFrames);
    viewWindowAt = now; viewWindowFrames = 0; viewWindowUs = 0;
  }
}

/* ── Home labels sit on the skin's artwork, not on a flat colour ────────────
 * osText() only adds glyphs; it never removes the old ones. At boot sdOk is
 * false, so "NO CARD" is drawn. When the card mounts a moment later "SD OK" is
 * drawn at the same right-aligned spot on top of it -- and the two strings
 * stay overprinted for good. The same happens to the effect name each time it
 * changes ("RAW" over "MONO"), and to either label whenever the card flaps.
 *
 * The artwork behind each label is captured when the art is painted, and put
 * back before the new text goes down. Nothing can restore it by reading the
 * glass back: this panel's MISO is dead, so reads return nothing. */

static void homeLabelRestore(int which) {            // 0 effect name, 1 ISO/EXP, 2 card state
  const HomeStrip& st = HOME_STRIPS[which];
  if (homeLblBgOk)
    lcd.pushImage(st.x, OS_BODY_Y, st.w, HOME_LBL_H, homeLblBg[which]);
  else
    lcd.fillRect(st.x, OS_BODY_Y, st.w, HOME_LBL_H, rgb565(SK.bg));
}

/* Called from osHomeFrame() the moment the art is painted, while it is still
   in viewBuf. (artW x artH) is the decoded size, srcY the first row shown, and
   the art is centred horizontally -- the same mapping osHomeFrame used. */
static void homeLabelCapture(const uint16_t* art, int artW, int artH, int srcY) {
  if (!art) { homeLblBgOk = false; return; }
  const int ox = (PW - artW) / 2;
  for (int which = 0; which < 3; ++which) {
    const int x0 = HOME_STRIPS[which].x, sw = HOME_STRIPS[which].w;
    for (int r = 0; r < HOME_LBL_H; ++r) {
      const int row = srcY + r;
      for (int c = 0; c < sw; ++c) {
        const int col = x0 + c - ox;
        homeLblBg[which][r * sw + c] =
          (row >= 0 && row < artH && col >= 0 && col < artW)
            ? art[(size_t)row * PW + col] : (uint16_t)rgb565(SK.bg);
      }
    }
  }
  homeLblBgOk = true;
}

static void osHomeHud(bool force = false) {
  static uint8_t lastFx = 255, lastIso = 255, lastExp = 255;
  static bool lastSd = false, lastRec = false;
  static int lastGal = -1, lastZoom = -1, lastAe = -9, lastContrast = -9;
  char buf[40];
  if (force || lastFx != uiFx || lastSd != sdOk || lastIso != osIso || lastExp != osExp) {
    // A forced draw follows a repaint of the art, which already cleared
    // everything; only a change on a live screen needs the old text removed.
    if (!force && lastFx != uiFx)                          homeLabelRestore(0);
    if (!force && (lastIso != osIso || lastExp != osExp))  homeLabelRestore(1);
    if (!force && lastSd != sdOk)                          homeLabelRestore(2);
    snprintf(buf, sizeof(buf), "%s", FX_NAME[uiFx]);
    osText(buf, 6, OS_BODY_Y + 1, rgb565(SK.accent));
    /* ISO and exposure, in the middle of the label row. Bright when something
       is held manual -- the one state a person needs to be reminded of. */
    char il[10], el[10]; lightName(true, osIso, il, sizeof(il)); lightName(false, osExp, el, sizeof(el));
    snprintf(buf, sizeof(buf), "ISO %s  EXP %s", il, el);
    osText(buf, PW / 2, OS_BODY_Y + 1, (osIso || osExp) ? rgb565(SK.accent) : inkOn(),
           textdatum_t::top_center);
    osText(sdOk ? "SD OK" : "NO CARD", PW - 6, OS_BODY_Y + 1,
           sdOk ? inkOn() : rgb565(SK.rec), textdatum_t::top_right);
    lastFx = uiFx; lastSd = sdOk; lastIso = osIso; lastExp = osExp;
  }
  if (force || lastGal != galN || lastRec != uiRec ||
      lastZoom != osZoomPct || lastAe != pstat.aeLevel ||
      lastContrast != pstat.contrast) {
    lcd.fillRect(0, HOME_VIEW_Y + HOME_VIEW_H, PW,
                 OS_BAR_Y - HOME_VIEW_Y - HOME_VIEW_H, rgb565(SK.bg));
    if (uiRec) {
      lcd.fillCircle(12, HOME_VIEW_Y + HOME_VIEW_H + 8, 3, rgb565(SK.rec));
      osText("REC", 21, HOME_VIEW_Y + HOME_VIEW_H + 3, rgb565(SK.rec));
    }
    snprintf(buf, sizeof(buf), "%.1fx E%+d C%+d", osZoomPct / 100.0f,
             (int)pstat.aeLevel, (int)pstat.contrast);
    osText(buf, PW / 2, HOME_VIEW_Y + HOME_VIEW_H + 3,
           rgb565(SK.accent), textdatum_t::top_center);
    char n[24];
    snprintf(n, sizeof(n), "%d ON CARD", galN);
    osText(n, PW - 5, HOME_VIEW_Y + HOME_VIEW_H + 3,
           inkOn(), textdatum_t::top_right);
    lastGal = galN; lastRec = uiRec; lastZoom = osZoomPct;
    lastAe = pstat.aeLevel; lastContrast = pstat.contrast;
  }
  if (force) {
    wuwPanel(HOME_VIEW_X + HOME_VIEW_W - 44, HOME_VIEW_Y + HOME_VIEW_H - 44,
             42, 42, false);
    osIcon("card", HOME_VIEW_X + HOME_VIEW_W - 31,
           HOME_VIEW_Y + HOME_VIEW_H - 31, rgb565(SK.text), 16);
  }
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
  bool compact = n >= 6;
  for (int i = 0; i < n; i++) {
    int y = OS_BODY_Y + 3 + i * rowH;
    wuwPanel(6, y, PW - 12, rowH - 4, i == osSel);
    osIcon(rows[i].icon, 16, y + (compact ? 3 : rowH / 2 - 12),
           i == osSel ? rgb565(SK.accent) : inkOn(), 16);
    osText(rows[i].label, 42, y + (compact ? 4 : rowH / 2 - 13),
           i == osSel ? rgb565(SK.accent) : rgb565(SK.text));
    if (!compact) osText(rows[i].sub, 42, y + rowH / 2 + 1, inkOn());
  }
}
static const ListRow MORE_ROWS[] = {
  {"aperture","CAMERA","size, quality, orientation", S_CAMERA},
  {"wifi","WIFI","join a network, keyboard", S_WIFI},
  {"sun","LIGHT","iso and exposure", S_LIGHT},
  {"mask","RITUAL GAMES","eight camera rituals", S_GAMES},
  {"people","ROOM","visitors and session", S_ROOM},
  {"gear","SKINS","nine material themes", S_SET},
  {"chip","SYSTEM","memory, storage, uptime", S_SYS},
};
#define MORE_N ((int)(sizeof(MORE_ROWS)/sizeof(MORE_ROWS[0])))

static const ListRow GAME_ROWS[] = {
  {"people","COLLECTIVE SHUTTER","linked viewpoints", S_COLLECTIVE},
  {"mask","GHOST RESIDUE","collect motion trails", S_GHOST},
  {"grid","APOPHENIA MACHINE","construct a perceived entity", S_APOPHENIA},
  {"eye","ORACLE","answer a question with an image", S_ORACLE},
  {"chip","BODY WITHOUT ORGANS","enter a first-person world", S_GAME},
  {"card","RITUAL RELAY","inherit and reinterpret", S_RELAY},
  {"sliders","EXPOSURE DIVINATION","tune concealed symbols", S_EXPOSURE},
  {"aperture","EIGHT GATES","camera ritual", S_GATES},
};
#define GAME_N ((int)(sizeof(GAME_ROWS)/sizeof(GAME_ROWS[0])))

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
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.35f);
  const int cols = 4, rows = (NSKIN + cols - 1) / cols;
  const int cw = PW / cols, ch = OS_BODY_H / rows;
  for (int i = 0; i < NSKIN; ++i) {
    int bx = (i % cols) * cw, by = OS_BODY_Y + (i / cols) * ch;
    const Skin& sample = SKINS[i];
    wuwPanelSkin(sample, bx + 2, by + 2, cw - 4, ch - 4, i == uiSkin);
    lcd.fillRect(bx + 8, by + 10, 18, 3, rgb(sample.accent));
    char n[3]; snprintf(n, sizeof(n), "%d", i + 1);
    osText(n, bx + cw - 8, by + 6, rgb(sample.accent), textdatum_t::top_right);
    osText(sample.name, bx + cw / 2, by + ch - 22, rgb(sample.text),
           textdatum_t::top_center);
    if (i == uiSkin)
      lcd.drawRect(bx + 4, by + 4, cw - 8, ch - 8, rgb(sample.accent));
  }
}

struct CameraRes { uint8_t id; const char* label; };
static const CameraRes CAM_RES[] = {
  {5,"QVGA 4:3"}, {8,"VGA 4:3"}, {9,"SVGA 4:3"}, {10,"XGA 4:3"},
  {11,"HD 16:9"}, {13,"UXGA 4:3"}, {14,"FHD 16:9"},
  {17,"QXGA 4:3"}, {18,"QHD 16:9"}, {21,"5MP 4:3"},
};
#define CAM_RES_N ((int)(sizeof(CAM_RES) / sizeof(CAM_RES[0])))
static const uint8_t LIVE_Q[] = { 8, 10, 12, 15, 20, 30, 40 };
static const uint8_t PHOTO_Q[] = { 4, 6, 8, 10, 12, 16, 20 };

static const char* resName(uint8_t id) {
  for (int i = 0; i < CAM_RES_N; ++i) if (CAM_RES[i].id == id) return CAM_RES[i].label;
  return "CUSTOM";
}
static int resNext(uint8_t id, bool still, uint8_t maxRes) {
  int first = -1;
  int last = still ? CAM_RES_N : 9;  // live omits 5MP; it is reserved for stills
  for (int i = 0; i < last; ++i) {
    if (CAM_RES[i].id > maxRes) continue;
    if (first < 0) first = i;
    if (CAM_RES[i].id == id) {
      for (int j = i + 1; j < last; ++j)
        if (CAM_RES[j].id <= maxRes) return CAM_RES[j].id;
      return CAM_RES[first].id;
    }
  }
  return first >= 0 ? CAM_RES[first].id : (uint8_t)FRAMESIZE_QVGA;
}
static int valueNext(const uint8_t* values, int count, uint8_t value) {
  for (int i = 0; i < count; ++i)
    if (values[i] == value) return values[(i + 1) % count];
  return values[0];
}
static void osCamera() {
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.35f);
  static const char* key[] = { "LIVE SIZE", "PHOTO SIZE", "LIVE QUALITY",
                               "PHOTO QUALITY", "BRIGHTNESS", "SENSOR",
                               "VIEW ROTATE" };
  char value[7][18];
  snprintf(value[0], sizeof(value[0]), "%s", resName(pstat.streamRes));
  snprintf(value[1], sizeof(value[1]), "%s", resName(pstat.photoRes));
  snprintf(value[2], sizeof(value[2]), "%u", pstat.streamQuality);
  snprintf(value[3], sizeof(value[3]), "%u", pstat.photoQuality);
  snprintf(value[4], sizeof(value[4]), "%+d", pstat.brightness);
  int orient = (pstat.hmirror ? 1 : 0) | (pstat.vflip ? 2 : 0);
  static const char* orientName[] = { "NORMAL", "MIRROR", "FLIP", "MIRROR+FLIP" };
  snprintf(value[5], sizeof(value[5]), "%s", orientName[orient]);
  snprintf(value[6], sizeof(value[6]), "%u DEG", (unsigned)osViewRot * 90);

  int rowH = OS_BODY_H / 7;
  for (int i = 0; i < 7; ++i) {
    int y = OS_BODY_Y + i * rowH;
    wuwPanel(5, y + 1, PW - 10, rowH - 2, false);
    osText(key[i], 12, y + 5, inkOn());
    osText(value[i], PW - 12, y + 5, rgb565(SK.accent), textdatum_t::top_right);
  }
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
static void osWifi();    // defined below with the keyboard
static void osLight();   // defined below with the ISO/exposure controls
static void osPresets() {
  static ListRow rows[4];
  int n = 0;
  rows[n++] = {"save","(none yet)","save a look from TUNE", S_MORE};
  osList(rows, n);
}

static int gateCurrent() {
  for (int i = 0; i < WUW_GATE_COUNT; ++i)
    if (!(gateMask & (1u << i))) return i;
  return WUW_GATE_COUNT;
}

static int gateOpenedCount() {
  int n = 0;
  for (int i = 0; i < WUW_GATE_COUNT; ++i) if (gateMask & (1u << i)) ++n;
  return n;
}

static void gateLoad() {
  if (gateLoaded) return;
  Preferences gp;
  gp.begin("wuwgates", true);
  gateMask = gp.getUChar("mask", 0);
  gp.end();
  gateLoaded = true;
}

static void gateSave() {
  Preferences gp;
  gp.begin("wuwgates", false);
  gp.putUChar("mask", gateMask);
  gp.end();
}

static void gateSay(const char* text, uint32_t duration = 1600) {
  snprintf(gateNotice, sizeof(gateNotice), "%s", text ? text : "");
  gateNoticeUntil = millis() + duration;
}

static void osGateHud() {
  gateLoad();
  int gate = gateCurrent();
  int opened = gateOpenedCount();
  int score = gate < WUW_GATE_COUNT && gateMetricsValid
            ? wuwGateScore(gate, gateMetrics, pstat.visitors) : 0;

  uint32_t sig = hudHashInt(2166136261u, (uint32_t)gate);
  sig = hudHashInt(sig, (uint32_t)opened);
  sig = hudHashInt(sig, (uint32_t)score);
  sig = hudHashInt(sig, (uint32_t)gateMask);
  sig = hudNoticeActive() ? hudHash(sig, gateNotice) : hudHashInt(sig, 0x5eedu);
  static uint32_t lastGateSig = 0;
  if (!hudForce && sig == lastGateSig) return;  // unchanged: leave the glass alone
  lastGateSig = sig; hudForce = false;

  wuwPanel(4, OS_BODY_Y + 4, PW - 8, 35, true);
  lcd.pushImage(7, OS_BODY_Y + 6, 32, 32, WuwGameArt::pixels + 6 * 1024, (uint16_t)0);
  char title[30];
  if (gate < WUW_GATE_COUNT)
    snprintf(title, sizeof(title), "GATE %d/8  %s", gate + 1, wuwGateName(gate));
  else
    snprintf(title, sizeof(title), "EIGHT GATES COMPLETE");
  osText(title, 43, OS_BODY_Y + 8, rgb565(SK.text));
  osText(gate < WUW_GATE_COUNT ? wuwGatePrompt(gate) : "THE ARCHIVE REMEMBERS",
         43, OS_BODY_Y + 23, rgb565(SK.accent));

  wuwPanel(4, OS_BODY_Y + OS_BODY_H - 31, PW - 8, 27, true);
  int dotX = 12;
  for (int i = 0; i < WUW_GATE_COUNT; ++i) {
    uint16_t c = (gateMask & (1u << i)) ? rgb565(SK.accent) : inkOn();
    if (i == gate) lcd.drawCircle(dotX + i * 15, OS_BODY_Y + OS_BODY_H - 18, 5, c);
    else           lcd.fillCircle(dotX + i * 15, OS_BODY_Y + OS_BODY_H - 18, 3, c);
  }
  int bx = 142, by = OS_BODY_Y + OS_BODY_H - 21, bw = 119;
  lcd.drawRect(bx, by, bw, 7, inkOn());
  lcd.fillRect(bx + 2, by + 2, (bw - 4) * score / 100, 3, rgb565(SK.accent));
  char progress[24]; snprintf(progress, sizeof(progress), "%d OPEN  %d%%", opened, score);
  osText(progress, PW - 8, OS_BODY_Y + OS_BODY_H - 27,
         rgb565(SK.text), textdatum_t::top_right);

  if (gateNotice[0] && (int32_t)(gateNoticeUntil - millis()) > 0) {
    wuwPanel(54, OS_BODY_Y + 69, PW - 108, 28, false);
    osText(gateNotice, PW / 2, OS_BODY_Y + 77, rgb565(SK.accent),
           textdatum_t::top_center);
  } else if (gateNotice[0]) {
    gateNotice[0] = 0;
  }
}

static uint16_t gateRow[PW];
static uint8_t divStage = 0;              // Exposure ritual: which symbol is being revealed
static int ritualScoreFor(int screen);    // defined with the ritual scoring below

/* What the ritual HUDs paint, in glass coordinates, each with its 2-px drop
   shadow. They are vertically disjoint, so a row crosses at most one of them. */
struct HudRect { int x, y, w, h; };
static int gateHudRects(HudRect* r) {
  int n = 0;
  r[n++] = { 4, OS_BODY_Y + 4, PW - 8, 37 };                         // top panel
  if (osScreen == S_GATES)                                           // gate progress
    r[n++] = { 4, OS_BODY_Y + OS_BODY_H - 31, PW - 8, 29 };
  else                                                               // ritual score
    r[n++] = { 4, OS_BODY_Y + OS_BODY_H - 27, PW - 8, 25 };
  if (hudNoticeActive())                                             // toast
    r[n++] = { 54, OS_BODY_Y + 69, PW - 108, 30 };
  return n;
}

/* The Exposure screen's symbol, as a pixel test. It used to be drawn on the
   glass after every frame, over the video; it is now part of the row itself.
   Shapes match the originals (circle, cross, eye, gate) to within a pixel. */
static bool exposureSymbolHit(int shape, int dx, int dy) {
  switch (shape & 3) {
    case 0: { int d2 = dx * dx + dy * dy; return d2 >= 132 && d2 <= 156; }
    case 1: return (dx == dy || dx == -dy) && dx >= -12 && dx <= 12;
    case 2: {
      if (dx * dx + dy * dy <= 9) return true;
      float e = (dx * dx) / 324.0f + (dy * dy) / 81.0f;
      return e >= 0.86f && e <= 1.14f;
    }
    default:
      if (dy < -16 || dy > 15 || dx < -13 || dx > 12) return false;
      return dx == -13 || dx == 12 || dy == -16 || dy == 15 || dx == 0;
  }
}

static void pushGateCamera(int srcW, int srcH) {
  if (!viewBuf || srcW <= 0 || srcH <= 0) return;
  int logicalW = (osViewRot & 1) ? srcH : srcW;
  int logicalH = (osViewRot & 1) ? srcW : srcH;
  int cropX = 0, cropY = 0, cropW = logicalW, cropH = logicalH;
  if ((int64_t)logicalW * OS_BODY_H > (int64_t)PW * logicalH) {
    cropW = (int)((int64_t)logicalH * PW / OS_BODY_H);
    cropX = (logicalW - cropW) / 2;
  } else {
    cropH = (int)((int64_t)logicalW * OS_BODY_H / PW);
    cropY = (logicalH - cropH) / 2;
  }
  cropW = max(1, cropW); cropH = max(1, cropH);

  HudRect hud[3];
  const int nHud = gateHudRects(hud);
  const bool symbol = osScreen == S_EXPOSURE;
  const int symShape = divStage & 3;
  const int symX = PW / 2, symY = OS_BODY_Y + OS_BODY_H / 2;
  const uint16_t symCol = ritualScoreFor(S_EXPOSURE) >= 80 ? rgb565(SK.accent) : inkOn();

  lcd.startWrite();
  for (int dy = 0; dy < OS_BODY_H; ++dy) {
    const int y = OS_BODY_Y + dy;
    int bx0 = 0, bx1 = 0;                       // span under a HUD panel, if any
    for (int k = 0; k < nHud; ++k)
      if (y >= hud[k].y && y < hud[k].y + hud[k].h) { bx0 = hud[k].x; bx1 = hud[k].x + hud[k].w; }
    int ly = cropY + (int)((int64_t)dy * cropH / OS_BODY_H);
    for (int dx = 0; dx < PW; ++dx) {
      if (dx >= bx0 && dx < bx1) { dx = bx1 - 1; continue; }   // HUD owns these pixels
      int lx = cropX + (int)((int64_t)dx * cropW / PW);
      int sx, sy;
      switch (osViewRot & 3) {
        case 1:  sx = ly;            sy = srcH - 1 - lx; break;
        case 2:  sx = srcW - 1 - lx; sy = srcH - 1 - ly; break;
        case 3:  sx = srcW - 1 - ly; sy = lx;            break;
        default: sx = lx;            sy = ly;             break;
      }
      sx = max(0, min(srcW - 1, sx));
      sy = max(0, min(srcH - 1, sy));
      gateRow[dx] = viewBuf[(size_t)sy * PW + sx];
    }
    if (symbol && y >= symY - 17 && y <= symY + 17)
      for (int dx = symX - 19; dx <= symX + 19; ++dx)
        if (exposureSymbolHit(symShape, dx - symX, y - symY)) gateRow[dx] = symCol;
    if (bx1 > bx0) {
      if (bx0 > 0)  lcd.pushImage(0, y, bx0, 1, gateRow);
      if (bx1 < PW) lcd.pushImage(bx1, y, PW - bx1, 1, gateRow + bx1);
    } else {
      lcd.pushImage(0, y, PW, 1, gateRow);
    }
  }
  lcd.endWrite();
}

static void osGates() {
  gateLoad();
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.55f);
  if (!viewBuf) {
    osText("CAMERA WORKSPACE UNAVAILABLE", PW / 2, OS_BODY_Y + 82,
           rgb565(SK.rec), textdatum_t::top_center);
  }
  osGateHud();
}

static void gateEnter() {
  gateLoad();
  gateMetricsValid = false;
  gateNotice[0] = 0;
  gateResetArmedUntil = 0;
  wuwGatesResetAnalysis();
}

static void gateResetRequest() {
  uint32_t now = millis();
  if ((int32_t)(gateResetArmedUntil - now) <= 0) {
    gateResetArmedUntil = now + 2200;
    gateSay("PRESS RESET AGAIN", 2200);
    return;
  }
  gateMask = 0;
  gateSave();
  gateMetricsValid = false;
  gateResetArmedUntil = 0;
  wuwGatesResetAnalysis();
  gateSay("RITUAL RESET");
}

static void gateScan() {
  gateLoad();
  if (uiRec) { gateSay("STOP RECORDING FIRST"); return; }
  int gate = gateCurrent();
  if (gate >= WUW_GATE_COUNT) { gateSay("RITUAL COMPLETE"); return; }
  if (!gateMetricsValid) { gateSay("WAIT FOR CAMERA"); return; }
  int score = wuwGateScore(gate, gateMetrics, pstat.visitors);
  if (score < 95) {
    char msg[24]; snprintf(msg, sizeof(msg), "SIGNAL %d%%", score);
    gateSay(msg);
    return;
  }

  gateMask |= (uint8_t)(1u << gate);
  gateSave();
  uiFx = (uint8_t)((gate + 1) % FX_COUNT);
  osChooseSkin((gate + 1) % NSKIN);
  char who[24]; snprintf(who, sizeof(who), "gate-%s", wuwGateName(gate));
  bool saved = fnShootFwd && fnShootFwd(who);
  gateMetricsValid = false;
  wuwGatesResetAnalysis();
  gateSay(saved ? "GATE OPEN / SAVED" : "GATE OPEN / NO SAVE", 2200);
}

/* ── Ritual suite ────────────────────────────────────────────────────────
   These games share the panel's one decoded camera frame. None allocates a
   second full image: Ghost reuses prevBuf, and the others transform viewBuf
   in place after their metrics have been sampled. */
static int ritualFrameW = 0, ritualFrameH = 0;
static char ritualSavedPath[80] = "";
static uint8_t ghostResidue = 0;
static bool ghostPrimed = false;
static uint32_t ghostLastGain = 0;
static uint8_t apoParts[6] = {};
static uint8_t apoPartCount = 0;
static uint8_t oracleStep = 0;
static uint16_t relayStage = 0;
static int relayGallery = -1;
static int divExposure = 300, divGain = 0;
static uint8_t ritualMask = 0;
static bool ritualProgressLoaded = false;

static void ritualProgressLoad() {
  if (ritualProgressLoaded) return;
  Preferences rp; rp.begin("ritual", true);
  ritualMask = rp.getUChar("mask", 0); rp.end();
  ritualProgressLoaded = true;
}

static void ritualComplete(uint8_t index) {
  ritualProgressLoad();
  ritualMask |= (uint8_t)(1u << index);
  Preferences rp; rp.begin("ritual", false); rp.putUChar("mask", ritualMask); rp.end();
}

static void ritualHud(const char* title, const char* prompt, int score,
                      const char* foot = nullptr) {
  uint32_t sig = hudHash(2166136261u, title);
  sig = hudHash(sig, prompt);
  sig = hudHash(sig, foot ? foot : "");
  sig = hudHashInt(sig, (uint32_t)constrain(score, 0, 100));
  sig = hudHashInt(sig, (uint32_t)osScreen);
  sig = hudNoticeActive() ? hudHash(sig, gateNotice) : hudHashInt(sig, 0x5eedu);
  static uint32_t lastSig = 0;
  if (!hudForce && sig == lastSig) return;      // unchanged: leave the glass alone
  lastSig = sig; hudForce = false;
  wuwPanel(4, OS_BODY_Y + 4, PW - 8, 35, true);
  int emblem = osScreen == S_GHOST ? 4 : osScreen == S_ORACLE ? 7 :
               osScreen == S_APOPHENIA ? 2 : osScreen == S_RELAY ? 0 : 5;
  lcd.pushImage(7, OS_BODY_Y + 6, 32, 32, WuwGameArt::pixels + emblem * 1024, (uint16_t)0);
  osText(title, 43, OS_BODY_Y + 8, rgb565(SK.text));
  osText(prompt, 43, OS_BODY_Y + 23, rgb565(SK.accent));
  wuwPanel(4, OS_BODY_Y + OS_BODY_H - 27, PW - 8, 23, true);
  int bx = 12, by = OS_BODY_Y + OS_BODY_H - 17, bw = 178;
  lcd.drawRect(bx, by, bw, 7, inkOn());
  lcd.fillRect(bx + 2, by + 2, (bw - 4) * constrain(score, 0, 100) / 100, 3,
               rgb565(SK.accent));
  char pct[12]; snprintf(pct, sizeof(pct), "%d%%", constrain(score, 0, 100));
  osText(foot && *foot ? foot : pct, PW - 10, OS_BODY_Y + OS_BODY_H - 22,
         rgb565(SK.text), textdatum_t::top_right);
  if (gateNotice[0] && (int32_t)(gateNoticeUntil - millis()) > 0) {
    wuwPanel(54, OS_BODY_Y + 69, PW - 108, 28, false);
    osText(gateNotice, PW / 2, OS_BODY_Y + 77, rgb565(SK.accent),
           textdatum_t::top_center);
  } else if (gateNotice[0]) gateNotice[0] = 0;
}

static bool saveRitualFrame(const char* kind) {
  ritualSavedPath[0] = 0;
  if (!sdOk || !viewBuf || ritualFrameW < 2 || ritualFrameH < 2) return false;
  uint8_t* jpg = nullptr;
  size_t jpgLen = 0;
  if (!fmt2jpg((uint8_t*)viewBuf,
               (size_t)ritualFrameW * ritualFrameH * sizeof(uint16_t),
               ritualFrameW, ritualFrameH, PIXFORMAT_RGB565, 86,
               &jpg, &jpgLen) || !jpg || !jpgLen) {
    if (jpg) free(jpg);
    return false;
  }
#ifdef EXHIBITION_MODE
  snprintf(ritualSavedPath, sizeof(ritualSavedPath), "%s/RITUAL_%s_%08lu.jpg",
           pasarSessionDir(), kind, (unsigned long)millis());
#else
  snprintf(ritualSavedPath, sizeof(ritualSavedPath), "/RITUAL_%s_%08lu.jpg",
           kind, (unsigned long)millis());
#endif
  File out = SD_MMC.open(ritualSavedPath, FILE_WRITE);
  bool ok = out && out.write(jpg, jpgLen) == jpgLen;
  if (out) out.close();
  free(jpg);
  if (!ok) {
    SD_MMC.remove(ritualSavedPath);
    ritualSavedPath[0] = 0;
    return false;
  }
  // Every completed ritual contributes to the same four-slot BWO material
  // library. Only the SD path is persisted; the 32x32 atlas is rebuilt lazily.
  bwoRememberWall(ritualSavedPath);
  galScan();
  Serial.printf("[RITUAL] saved %s (%u KB)\n", ritualSavedPath,
                (unsigned)(jpgLen / 1024));
  return true;
}

static int ritualScoreFor(int screen) {
  if (!gateMetricsValid) return 0;
  if (screen == S_GHOST) return constrain((int)gateMetrics.motion * 5, 0, 100);
  if (screen == S_APOPHENIA)
    return constrain((int)gateMetrics.edge * 3 + (int)gateMetrics.contrast, 0, 100);
  if (screen == S_EXPOSURE) {
    static const int targets[] = { 58, 92, 128, 168 };
    int light = 100 - abs((int)gateMetrics.luma - targets[divStage & 3]);
    int detail = constrain((int)gateMetrics.contrast * 2, 0, 100);
    return constrain((light * 3 + detail) / 4, 0, 100);
  }
  return 100;
}

static void ghostReset() {
  ghostResidue = 0; ghostPrimed = false; ghostLastGain = 0;
  if (prevBuf) memset(prevBuf, 0, (size_t)PW * PH * sizeof(uint16_t));
  wuwGatesResetAnalysis();
}

static void ghostFrame(uint16_t* pixels, int w, int h, int stride) {
  if (!pixels || !prevBuf) return;
  if (!ghostPrimed) {
    memcpy(prevBuf, pixels, (size_t)stride * h * sizeof(uint16_t));
    ghostPrimed = true;
    return;
  }
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      size_t i = (size_t)y * stride + x;
      uint16_t cur = pixels[i], old = prevBuf[i];
      int cr = (cur >> 11) & 31, cg = (cur >> 5) & 63, cb = cur & 31;
      int orr = (old >> 11) & 31, og = (old >> 5) & 63, ob = old & 31;
      int delta = abs((int)panelFxLuma(cur) - (int)panelFxLuma(old));
      int r = max(cr * 3 / 4, orr * 5 / 6);
      int g = max(cg * 3 / 4, og * 5 / 6);
      int b = max(cb * 3 / 4, ob * 5 / 6);
      if (delta > 14) { g = min(63, g + delta / 8); b = min(31, b + delta / 12); }
      uint16_t trail = (uint16_t)((r << 11) | (g << 5) | b);
      pixels[i] = trail;
      prevBuf[i] = trail;
    }
  }
  uint32_t now = millis();
  if (now - ghostLastGain > 260 && gateMetrics.motion >= 5) {
    ghostLastGain = now;
    ghostResidue = min(100, (int)ghostResidue + max(1, (int)gateMetrics.motion / 3));
  }
}

static void putRitualPixel(uint16_t* p, int w, int h, int stride,
                           int x, int y, uint16_t c) {
  if ((unsigned)x < (unsigned)w && (unsigned)y < (unsigned)h)
    p[(size_t)y * stride + x] = c;
}

static void ritualLine(uint16_t* p, int w, int h, int stride,
                       int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    putRitualPixel(p, w, h, stride, x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = err * 2;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

static void apopheniaFrame(uint16_t* pixels, int w, int h, int stride) {
  if (!pixels || !prevBuf) return;
  memcpy(prevBuf, pixels, (size_t)stride * h * sizeof(uint16_t));
  uint16_t ink = rgb565(SK.accent), hot = rgb565(SK.rec);
  for (int y = 1; y < h; ++y) for (int x = 1; x < w; ++x) {
    size_t i = (size_t)y * stride + x;
    int l = panelFxLuma(prevBuf[i]);
    int e = abs(l - (int)panelFxLuma(prevBuf[i - 1])) +
            abs(l - (int)panelFxLuma(prevBuf[i - stride]));
    pixels[i] = e > 48 ? (e > 100 ? hot : ink) : rgb565(SK.bg);
  }
  int cx = w / 2, cy = h / 2;
  for (int i = 0; i < apoPartCount; ++i) {
    int span = 14 + i * 8, lift = (i & 1) ? -span : span;
    switch (apoParts[i] % 3) {
      case 0:
        ritualLine(pixels,w,h,stride,cx-span,cy-lift/2,cx+span,cy+lift/2,ink);
        ritualLine(pixels,w,h,stride,cx-span,cy+lift/2,cx+span,cy-lift/2,ink); break;
      case 1:
        ritualLine(pixels,w,h,stride,cx,cy-span,cx-span,cy+span,hot);
        ritualLine(pixels,w,h,stride,cx-span,cy+span,cx+span,cy+span,hot);
        ritualLine(pixels,w,h,stride,cx+span,cy+span,cx,cy-span,hot); break;
      default:
        ritualLine(pixels,w,h,stride,cx-span,cy,cx,cy-span,ink);
        ritualLine(pixels,w,h,stride,cx,cy-span,cx+span,cy,ink);
        ritualLine(pixels,w,h,stride,cx+span,cy,cx,cy+span,ink);
        ritualLine(pixels,w,h,stride,cx,cy+span,cx-span,cy,ink); break;
    }
  }
}

static void apoChoose(uint8_t part) {
  if (apoPartCount < sizeof(apoParts)) apoParts[apoPartCount++] = part % 3;
  else gateSay("ENTITY COMPLETE");
}

static const char* oraclePrompt() {
  static const char* prompts[] = {
    "FIND AN ARTIFICIAL MOON", "CAPTURE SOMETHING BECOMING LIQUID",
    "FRAME A DOOR THAT REFUSES YOU", "FIND A SHADOW WITH NO OWNER",
    "PHOTOGRAPH A BORROWED MEMORY", "SHOW WHERE THE MARKET DREAMS",
    "FIND A MACHINE PRETENDING TO LIVE", "CAPTURE THE QUIETEST COLOR",
  };
  uint32_t hash = 2166136261u;
  const char* text = pstat.session;
  for (const char* s = text ? text : ""; *s; ++s) hash = (hash ^ (uint8_t)*s) * 16777619u;
  return prompts[(hash + oracleStep) % (sizeof(prompts) / sizeof(prompts[0]))];
}

static void relayLoad() {
  Preferences rp; rp.begin("ritual", true);
  relayStage = rp.getUShort("relay", 0); rp.end();
  if (sdOk) galScan();
  relayGallery = -1;
  for (int i = 0; i < galN; ++i) if (!galVid[i]) { relayGallery = i; break; }
}

static void exposureApply() {
  if (!fnCameraFwd) return;
  fnCameraFwd(PANEL_CAM_AEC, 0);
  fnCameraFwd(PANEL_CAM_AGC, 0);
  fnCameraFwd(PANEL_CAM_AEC_VALUE, divExposure);
  fnCameraFwd(PANEL_CAM_AGC_GAIN, divGain);
}

static void lightApply(bool saveIt);       // the LIGHT screen's ISO / exposure, defined below
static void exposureLeave() {
  if (!fnCameraFwd) return;
  fnCameraFwd(PANEL_CAM_AEC, 1);
  fnCameraFwd(PANEL_CAM_AGC, 1);
  /* Back to auto is right for the ritual's own exit, but not for a person who
     had set a manual ISO or shutter on the LIGHT screen: that would be dropped
     silently while the Home label still said manual. Put it back. */
  lightApply(false);
}

static void osCollective() {
  osInfoPanel();
  lcd.pushImage(12, OS_BODY_Y + 7, 32, 32, WuwGameArt::pixels, (uint16_t)0);
  osText("COLLECTIVE SHUTTER RITUAL", 50, OS_BODY_Y + 12, rgb565(SK.text));
  char b[28]; snprintf(b, sizeof(b), "%u LINKED", (unsigned)pstat.peers);
  osKV("CAMERAS", b, 3);
  osKV("SESSION", pstat.session && *pstat.session ? pstat.session : "-", 4);
  osText(pstat.peers ? "SYNC FIRES EVERY VIEWPOINT" : "WAITING FOR WUW PEERS",
         PW / 2, OS_BODY_Y + OS_BODY_H - 24,
         pstat.peers ? rgb565(SK.accent) : inkOn(), textdatum_t::top_center);
  if (gateNotice[0] && (int32_t)(gateNoticeUntil - millis()) > 0)
    osText(gateNotice, PW / 2, OS_BODY_Y + 58, rgb565(SK.rec), textdatum_t::top_center);
}

static void osGames() {
  ritualProgressLoad();
  osList(GAME_ROWS, GAME_N);
  int rowH = (OS_BODY_H - 6) / GAME_N;
  for (int i = 0; i < 7; ++i) {
    int cy = OS_BODY_Y + 3 + i * rowH + rowH / 2;
    if (ritualMask & (1u << i))
      lcd.fillCircle(PW - 18, cy, 3, rgb565(SK.accent));
    else
      lcd.drawCircle(PW - 18, cy, 3, inkOn());
  }
}

static void osGhost() {
  char foot[20]; snprintf(foot, sizeof(foot), "%u RESIDUE", (unsigned)ghostResidue);
  ritualHud("GHOST RESIDUE", "MOVE, LEAVE A TRACE, COLLECT", ghostResidue, foot);
}

static void osApophenia() {
  char title[32]; snprintf(title, sizeof(title), "APOPHENIA ENTITY %u/6", apoPartCount);
  ritualHud(title, "I CROSS  II CROWN  III DIAMOND",
            ritualScoreFor(S_APOPHENIA), "CHOOSE WHAT YOU SEE");
}

static void osOracle() {
  char src[34];
  snprintf(src, sizeof(src), "ORACLE  QUESTION %u", (unsigned)(oracleStep + 1));
  ritualHud(src, oraclePrompt(), 100, "PHOTO IS THE ANSWER");
}

static void osRelay() {
  static const char* acts[] = { "CHANGE THE ANGLE", "INVERT THE SUBJECT",
    "REMOVE THE BODY", "REPEAT ONE COLOR", "MOVE CLOSER", "BREAK THE HORIZON" };
  char title[38];
  if (relayGallery >= 0) snprintf(title, sizeof(title), "INHERIT %.26s", galName[relayGallery]);
  else snprintf(title, sizeof(title), "NO PREVIOUS PHOTO");
  ritualHud(title, acts[relayStage % (sizeof(acts) / sizeof(acts[0]))],
            100, "SHOOT TO CONTINUE CHAIN");
}

static void osExposure() {
  int score = ritualScoreFor(S_EXPOSURE);
  static const char* symbols[] = { "CIRCLE", "CROSS", "EYE", "GATE" };
  char title[38], foot[28];
  snprintf(title, sizeof(title), "DIVINATION: REVEAL %s", symbols[divStage & 3]);
  snprintf(foot, sizeof(foot), "EXP %d  ISO %d", divExposure, divGain);
  ritualHud(title, "TAP LOWER FIELD TO SET ISO", score, foot);
  if (pstat.cam) return;            // with a live picture the symbol is part of each pushed row
  uint16_t c = score >= 80 ? rgb565(SK.accent) : inkOn();
  int cx = PW / 2, cy = OS_BODY_Y + OS_BODY_H / 2;
  if ((divStage & 3) == 0) lcd.drawCircle(cx, cy, 12, c);
  else if ((divStage & 3) == 1) { lcd.drawLine(cx-12,cy-12,cx+12,cy+12,c); lcd.drawLine(cx+12,cy-12,cx-12,cy+12,c); }
  else if ((divStage & 3) == 2) { lcd.drawEllipse(cx,cy,18,9,c); lcd.fillCircle(cx,cy,3,c); }
  else { lcd.drawRect(cx-13,cy-16,26,32,c); lcd.drawFastVLine(cx,cy-16,32,c); }
}

static void osCard() {
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.40f);
  constexpr int CW = PW / 3, CH = OS_BODY_H / 2;
  constexpr int TW = CW - 12, TH = CH - 22;
  static uint16_t* thumbs = nullptr;
  static int cached[6] = {-1, -1, -1, -1, -1, -1};
  static uint8_t widths[6] = {}, heights[6] = {};
  static uint32_t cacheRevision = 0;
  if (!thumbs)
    thumbs = (uint16_t*)heap_caps_malloc((size_t)6 * TW * TH * 2,
                                         MALLOC_CAP_SPIRAM);
  if (cacheRevision != galRevision) {
    for (int& item : cached) item = -1;
    cacheRevision = galRevision;
  }
  for (int k = 0; k < 6; k++) {
    int idx = galTop + k;
    int bx = (k % 3) * CW, by = OS_BODY_Y + (k / 3) * CH;
    wuwPanel(bx + 2, by + 2, CW - 4, CH - 4, idx == galSel);
    if (idx >= galN) continue;
    if (thumbs && !galVid[idx]) {
      if (cached[k] != idx) {
        int tw = 0, th = 0;
        bool ok = decodeFileInto(galName[idx], thumbs + (size_t)k * TW * TH,
                                 TW, TH, &tw, &th);
        widths[k] = ok ? tw : 0;
        heights[k] = ok ? th : 0;
        cached[k] = idx;
      }
      if (widths[k] && heights[k]) {
        int ox = bx + (CW - widths[k]) / 2;
        int oy = by + (CH - 18 - heights[k]) / 2;
        uint16_t* src = thumbs + (size_t)k * TW * TH;
        for (int r = 0; r < heights[k]; ++r)
          lcd.pushImage(ox, oy + r, widths[k], 1, src + (size_t)r * TW);
      }
    } else if (galVid[idx]) {
      osIcon("rec", bx + CW / 2 - 8, by + CH / 2 - 13,
             rgb565(SK.accent), 16);
    }
    osText(galName[idx], bx + CW / 2, by + CH - 17,
           idx == galSel ? rgb565(SK.accent) : inkOn(), textdatum_t::top_center);
    if (galVid[idx])
      lcd.fillTriangle(bx + 8, by + 8, bx + 8, by + 17, bx + 16, by + 12.5f,
                       rgb565(SK.accent));
  }
  if (cardNotice[0] && (int32_t)(cardNoticeUntil - millis()) > 0) {
    wuwPanel(62, OS_BODY_Y + 70, PW - 124, 28, false);
    osText(cardNotice, PW / 2, OS_BODY_Y + 78, rgb565(SK.accent),
           textdatum_t::top_center);
  } else if (cardNotice[0]) cardNotice[0] = 0;
}

static void osBwoLibrary() {
  osInfoPanel();
  bwoLoadWalls();
  if (!bwoGameBegin(sdOk)) return;
  if(!bwoPatchScanned) {
    for(int i=0;i<4;++i)bwoPatchSaved[i]=bwoGamePatchExists(i);
    bwoPatchScanned=true;
  }
  for (int i=0;i<4;i++) {
    int y=OS_BODY_Y+4+i*30;
    wuwPanel(6,y,PW-12,27,i==bwoPatchSelection);
    char text[40]; snprintf(text,sizeof(text),"PATCH %d   %s",i+1,bwoPatchSaved[i]?"SAVED WORLD":"EMPTY");
    osText(text,16,y+8,i==bwoPatchSelection?rgb565(SK.accent):rgb565(SK.text));
  }
  osText("CONTINUE CURRENT WORLD",PW/2,OS_BODY_Y+136,rgb565(SK.accent),textdatum_t::top_center);
  if (gateNotice[0] && (int32_t)(gateNoticeUntil-millis())>0)
    osText(gateNotice,PW/2,OS_BODY_Y+153,rgb565(SK.rec),textdatum_t::top_center);
}

/* The style screen's rows. Drawing and touch both use these, so they cannot drift apart:
   theme, UV turn and glitch cycle when tapped; fragments is a read-out; the name opens the keyboard. */
#define BWO_STYLE_ROWS   5
#define BWO_STYLE_ROW_Y  4
#define BWO_STYLE_PITCH  27
#define BWO_STYLE_ROW_H  25
#define BWO_STYLE_NAME_ROW 4

static void osBwoStyle() {
  osInfoPanel();
  static const char* themes[]={"SHRINE","CHROME","THORN","TENDRIL"};
  char labels[BWO_STYLE_ROWS][48];
  snprintf(labels[0],48,"THEME     %s",themes[bwoGameTheme()]);
  snprintf(labels[1],48,"UV TURN   %u DEG",(unsigned)bwoGameUv()*90);
  snprintf(labels[2],48,"GLITCH    %u / 3",(unsigned)bwoGameGlitch());
  snprintf(labels[3],48,"FRAGMENTS %u / 7",(unsigned)bwoGameFragments());
  snprintf(labels[BWO_STYLE_NAME_ROW],48,"NAME      %s",bwoGameName());
  for(int i=0;i<BWO_STYLE_ROWS;i++) {
    int y=OS_BODY_Y+BWO_STYLE_ROW_Y+i*BWO_STYLE_PITCH;wuwPanel(6,y,PW-12,BWO_STYLE_ROW_H,false);
    osText(labels[i],16,y+8,i==BWO_STYLE_NAME_ROW?rgb565(SK.accent):rgb565(SK.text));
    if(i==BWO_STYLE_NAME_ROW) osText("TAP TO CHANGE",PW-14,y+8,inkOn(),textdatum_t::top_right);
  }
  if (gateNotice[0] && (int32_t)(gateNoticeUntil-millis())>0)
    osText(gateNotice,PW/2,OS_BODY_Y+148,rgb565(SK.rec),textdatum_t::top_center);
}

static void osBwoManage() {
  osInfoPanel();
  osText("RESET CURRENT WORLD",PW/2,OS_BODY_Y+38,rgb565(SK.rec),textdatum_t::top_center);
  osText("SAVED PATCHES ARE KEPT",PW/2,OS_BODY_Y+65,rgb565(SK.text),textdatum_t::top_center);
  wuwPanel(48,OS_BODY_Y+92,PW-96,38,false);
  osText((int32_t)(bwoResetUntil-millis())>0?"PRESS CONFIRM BELOW":"ARM RESET",PW/2,OS_BODY_Y+104,
    rgb565(SK.accent),textdatum_t::top_center);
}

static void osBwoPhotos() {
  osInfoPanel();
  bwoLoadWalls();
  uint8_t count=bwoGameWallTextureCount();
  for(int slot=0;slot<BWO_WALL_TEXTURE_SLOTS;++slot) {
    int y=OS_BODY_Y+4+slot*34;
    bool selected=slot==bwoGameSelectedWallTexture() && slot<count;
    wuwPanel(6,y,PW-12,32,selected);
    const uint16_t* pixels=bwoGameWallTexture(slot);
    if(pixels) lcd.pushImage(10,y,32,32,pixels);
    char label[24];
    snprintf(label,sizeof(label),"PHOTO %d  %s",slot+1,pixels?(selected?"SELECTED":"TAP TO SELECT"):"EMPTY");
    osText(label,50,y+10,selected?rgb565(SK.accent):rgb565(SK.text));
  }
  osText("ADD FROM GALLERY",PW/2,OS_BODY_Y+147,rgb565(SK.accent),textdatum_t::top_center);
}

/* ── Game overlays: composited into the frame, never drawn over it ─────────
 * The toolbar used to be repainted on the glass after EVERY 3D frame -- a
 * 320x168 push that wiped it, then fills, outlines and four labels redrawn on
 * top, twenty times a second. The result is shimmering text. It is now drawn
 * into a small sprite only when its state changes, and copied into the rows of
 * each frame before they are pushed.
 *
 * The toast is the other half. gateSay() set a message ("FACE PAINTED", "AIM AT
 * A WALL", "WORLD CHANGED") that the game screen never drew, so every build and
 * paint action gave no feedback whether it worked or was refused. */
#define BWO_TOOL_H   21
#define BWO_TOAST_W  224
#define BWO_TOAST_H  24
#define BWO_TOAST_Y  122          // body-relative: lower third of the 3D view
/* The player's name, as a small tag under the portrait box the game draws at the top left
   (that box is 144 x 34 body pixels at (6, 28)). The widest name is BWO_NAME_MAX characters of
   the 6 px font plus a margin; shorter names get a shorter tag. */
#define BWO_NAME_W   (BWO_NAME_MAX * 6 + 6)
#define BWO_NAME_H   13
#define BWO_NAME_X   6
#define BWO_NAME_Y   64
static lgfx::LGFX_Sprite bwoToolSpr(&lcd);
static lgfx::LGFX_Sprite bwoToastSpr(&lcd);
static lgfx::LGFX_Sprite bwoNameSpr(&lcd);
static uint16_t* bwoToolPix  = nullptr;
static uint16_t* bwoToastPix = nullptr;
static uint16_t* bwoNamePix  = nullptr;
static uint32_t  bwoToolSig  = 0xFFFFFFFFu;
static uint32_t  bwoToastSig = 0;
static uint32_t  bwoNameSig  = 0;
static int       bwoNameTagW = 0;

static bool bwoOverlayInit() {
  static bool failed = false;       // try once; never spam the log at frame rate
  if (bwoToolPix && bwoToastPix && bwoNamePix) return true;
  if (failed) return false;
  failed = true;                    // cleared below only if everything succeeds
  bwoToolSpr.setColorDepth(16);  bwoToolSpr.setPsram(true);
  bwoToastSpr.setColorDepth(16); bwoToastSpr.setPsram(true);
  bwoNameSpr.setColorDepth(16);  bwoNameSpr.setPsram(true);
  if (!bwoToolSpr.createSprite(PW, BWO_TOOL_H) ||
      !bwoToastSpr.createSprite(BWO_TOAST_W, BWO_TOAST_H) ||
      !bwoNameSpr.createSprite(BWO_NAME_W, BWO_NAME_H)) {
    Serial.println("[BWO] overlay sprites unavailable - toolbar, name tag and toasts off");
    return false;
  }
  bwoToolPix  = (uint16_t*)heap_caps_malloc((size_t)PW * BWO_TOOL_H * 2, MALLOC_CAP_SPIRAM);
  bwoToastPix = (uint16_t*)heap_caps_malloc((size_t)BWO_TOAST_W * BWO_TOAST_H * 2, MALLOC_CAP_SPIRAM);
  bwoNamePix  = (uint16_t*)heap_caps_malloc((size_t)BWO_NAME_W * BWO_NAME_H * 2, MALLOC_CAP_SPIRAM);
  if (!bwoToolPix || !bwoToastPix || !bwoNamePix) {
    Serial.println("[BWO] overlay buffers unavailable - toolbar, name tag and toasts off");
    return false;
  }
  failed = false;
  return true;
}

static void bwoNameRender() {
  const char* name = bwoGameName();
  uint32_t sig = 2166136261u;
  for (const char* c = name; *c; ++c) sig = (sig ^ (uint8_t)*c) * 16777619u;
  sig = (sig ^ (uint32_t)(uintptr_t)SK.name) | 1u;   // a new skin repaints the tag too
  if (sig == bwoNameSig) return;                     // nothing changed: no work
  bwoNameSig = sig;
  bwoNameSpr.setTextSize(1);
  int w = bwoNameSpr.textWidth(name) + 8;
  if (w > BWO_NAME_W) w = BWO_NAME_W;
  bwoNameTagW = w;
  bwoNameSpr.fillRect(0, 0, BWO_NAME_W, BWO_NAME_H, bwoNameSpr.color565(4, 10, 12));
  bwoNameSpr.drawRect(0, 0, w, BWO_NAME_H, rgb565(SK.accent));
  osTextOn(bwoNameSpr, name, w / 2, 3, rgb565(SK.accent), textdatum_t::top_center, false);
  bwoNameSpr.readRect(0, 0, BWO_NAME_W, BWO_NAME_H, bwoNamePix);
}

static void bwoToolsRender() {
  static const char* faces[] = { "WALL", "WALL", "WALL", "WALL", "BOTTOM", "TOP" };
  const bool build = bwoGameBuildMode();
  uint32_t sig = (uint32_t)bwoGameLevel() | ((build ? 1u : 0u) << 4) |
                 ((uint32_t)bwoGameSelectedFace() << 5) |
                 ((uint32_t)(uintptr_t)SK.name << 8);
  if (sig == bwoToolSig) return;                    // nothing changed: no work
  bwoToolSig = sig;
  const int h = BWO_TOOL_H, w = PW / 4;
  bwoToolSpr.fillRect(0, 0, PW, h, bwoToolSpr.color565(4, 10, 12));
  char down[12], up[12];
  snprintf(down, sizeof(down), "DOWN %u", (unsigned)bwoGameLevel() + 1);
  snprintf(up, sizeof(up), "UP %u", (unsigned)bwoGameLevel() + 1);
  const char* labels[] = { down, up, faces[bwoGameSelectedFace()], build ? "PAINT" : "BUILD" };
  for (int i = 0; i < 4; ++i) {
    bwoToolSpr.drawRect(i * w, 0, w - 1, h, inkOn());
    osTextOn(bwoToolSpr, labels[i], i * w + w / 2, 3,
             (i < 2 || build) ? rgb565(SK.text) : inkOn(),
             textdatum_t::top_center, false);
  }
  bwoToolSpr.readRect(0, 0, PW, h, bwoToolPix);
}

static bool bwoToastActive() {
  return gateNotice[0] && (int32_t)(gateNoticeUntil - millis()) > 0;
}

static void bwoToastRender() {
  uint32_t sig = 0;
  if (bwoToastActive()) {
    sig = 2166136261u;
    for (const char* c = gateNotice; *c; ++c) sig = (sig ^ (uint8_t)*c) * 16777619u;
    sig |= 1u;
  }
  if (sig == bwoToastSig) return;
  bwoToastSig = sig;
  if (!sig) return;
  bwoToastSpr.fillRect(0, 0, BWO_TOAST_W, BWO_TOAST_H, bwoToastSpr.color565(4, 10, 12));
  bwoToastSpr.drawRect(0, 0, BWO_TOAST_W, BWO_TOAST_H, rgb565(SK.accent));
  bwoToastSpr.drawRect(1, 1, BWO_TOAST_W - 2, BWO_TOAST_H - 2, inkOn());
  osTextOn(bwoToastSpr, gateNotice, BWO_TOAST_W / 2, 8, rgb565(SK.accent),
           textdatum_t::top_center, false);
  bwoToastSpr.readRect(0, 0, BWO_TOAST_W, BWO_TOAST_H, bwoToastPix);
}

static void osBwoFrame(uint32_t now) {
  const uint16_t* game = bwoGameFrame(now);
  if (!game) return;
  const bool overlays = bwoOverlayInit();
  if (overlays) { bwoToolsRender(); bwoNameRender(); bwoToastRender(); }
  const bool toast = overlays && bwoToastActive() && bwoToastSig;
  static uint16_t rows[PW * 8];
  lcd.startWrite();
  for (int sy = 0; sy < BWO_FB_H; sy += 4) {
    int count = min(4, BWO_FB_H - sy);
    for (int r = 0; r < count; ++r) {
      uint16_t* dst = rows + (size_t)r * PW * 2;
      const uint16_t* src = game + (size_t)(sy + r) * BWO_FB_W;
      for (int sx = 0; sx < BWO_FB_W; ++sx) {
        dst[sx * 2] = src[sx];
        dst[sx * 2 + 1] = src[sx];
      }
      memcpy(dst + PW, dst, PW * sizeof(uint16_t));
    }
    if (overlays) {
      // Row j of this chunk is body row (sy * 2 + j). Overlays replace those
      // pixels in the buffer, so the glass never sees the bare 3D frame under them.
      for (int j = 0; j < count * 2; ++j) {
        int by = sy * 2 + j;
        uint16_t* line = rows + (size_t)j * PW;
        if (by < BWO_TOOL_H)
          memcpy(line, bwoToolPix + (size_t)by * PW, PW * sizeof(uint16_t));
        if (by >= BWO_NAME_Y && by < BWO_NAME_Y + BWO_NAME_H && bwoNameTagW > 0)
          memcpy(line + BWO_NAME_X,
                 bwoNamePix + (size_t)(by - BWO_NAME_Y) * BWO_NAME_W,
                 bwoNameTagW * sizeof(uint16_t));
        if (toast && by >= BWO_TOAST_Y && by < BWO_TOAST_Y + BWO_TOAST_H)
          memcpy(line + (PW - BWO_TOAST_W) / 2,
                 bwoToastPix + (size_t)(by - BWO_TOAST_Y) * BWO_TOAST_W,
                 BWO_TOAST_W * sizeof(uint16_t));
      }
    }
    lcd.pushImage(0, OS_BODY_Y + sy * 2, PW, count * 2, rows);
  }
  lcd.endWrite();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  ON-SCREEN KEYBOARD
 *  A pop-up that takes over the screen below the status strip, used for the
 *  WiFi network name and password. Four layers (abc, ABC, 123, #+=) reach every
 *  printable ASCII character, which a WiFi password can use.
 *
 *  Resistive glass is slow and imprecise, so:
 *   - a key ACTS ON PRESS, with its highlight drawn at once; waiting for release
 *     on every letter makes typing feel broken;
 *   - after a press nothing else fires until the finger lifts, so one touch can
 *     never type a letter twice;
 *   - key hit boxes extend a couple of pixels past the drawn key.
 *
 *  Every edit redraws only what changed (one key, or the text field). The keys
 *  themselves are drawn once per layer.
 * ═══════════════════════════════════════════════════════════════════════════ */
enum { KB_NONE = 0, KB_SSID, KB_PASS, KB_NAME };
enum { K_CHAR = 0, K_SHIFT, K_SYM, K_BKSP, K_SPACE, K_SHOW, K_OK, K_CANCEL, K_ABC };
struct KbKey { int16_t x, y, w, h; uint8_t kind; char ch; };

#define KB_ROW_Y   62
#define KB_ROW_H   36
#define KB_KEY_W   32
#define KB_FIELD_Y 34
static struct {
  bool    active;
  uint8_t purpose;
  char    title[44];
  char    text[66];
  uint8_t len, maxLen;
  bool    mask, reveal, fingerDown;
  uint8_t layer;                // 0 abc, 1 ABC, 2 123, 3 #+=
  int8_t  pressed;
} kb = {};
static KbKey   kbKeys[44];
static uint8_t kbN = 0;
static char    wifiPendingSsid[34] = "";     // set while the password is being typed

static const char* const KB_LAYER[4][3] = {
  { "qwertyuiop", "asdfghjkl",  "zxcvbnm" },
  { "QWERTYUIOP", "ASDFGHJKL",  "ZXCVBNM" },
  { "1234567890", "-/:;()$&@\"", ".,?!'_~" },
  { "[]{}#%^*+=", "\\|<>`",     ".,?!'-_" },
};

static void kbBuild() {
  kbN = 0;
  auto add = [&](int x, int y, int w, uint8_t kind, char ch) {
    if (kbN < 44) kbKeys[kbN++] = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)KB_ROW_H, kind, ch };
  };
  for (int r = 0; r < 3; ++r) {
    const char* row = KB_LAYER[kb.layer][r];
    int n = (int)strlen(row), y = KB_ROW_Y + r * KB_ROW_H;
    if (r < 2) {                                         // centred rows
      int x0 = (PW - n * KB_KEY_W) / 2;
      for (int i = 0; i < n; ++i) add(x0 + i * KB_KEY_W, y, KB_KEY_W, K_CHAR, row[i]);
    } else {                                             // shift + letters + delete
      add(0, y, 48, kb.layer >= 2 ? K_SYM : K_SHIFT, 0);
      for (int i = 0; i < n; ++i) add(48 + i * KB_KEY_W, y, KB_KEY_W, K_CHAR, row[i]);
      add(PW - 48, y, 48, K_BKSP, 0);
    }
  }
  int y = KB_ROW_Y + 3 * KB_ROW_H;
  add(0,   y, 48,  kb.layer >= 2 ? K_ABC : K_SYM, 0);
  add(48,  y, 48,  K_SHOW, 0);
  add(96,  y, 128, K_SPACE, ' ');
  add(224, y, 48,  K_CANCEL, 0);
  add(272, y, 48,  K_OK, 0);
}

static void kbLabelOn(const KbKey& k, bool big, const char* text, uint16_t col) {
  lcd.setTextSize(big ? 2 : 1);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(col);
  lcd.drawString(text, k.x + k.w / 2, k.y + k.h / 2);
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextSize(1);
}

static void kbDrawKey(int i, bool pressed) {
  const KbKey& k = kbKeys[i];
  bool lit = pressed;
  const char* label = nullptr; char one[2] = { k.ch, 0 };
  bool big = false;
  switch (k.kind) {
    case K_CHAR:  label = one; big = true; break;
    case K_SPACE: label = "SPACE"; break;
    case K_SHIFT: label = "SHIFT"; lit = lit || kb.layer == 1; break;
    case K_SYM:   label = kb.layer >= 2 ? "#+=" : "123";
                  if (kb.layer == 3) label = "123"; else if (kb.layer == 2) label = "#+=";
                  break;
    case K_ABC:   label = "ABC"; break;
    case K_BKSP:  label = "DEL"; break;
    case K_SHOW:  label = kb.reveal ? "HIDE" : "SHOW"; lit = lit || (kb.mask && kb.reveal); break;
    case K_OK:    label = "OK"; break;
    case K_CANCEL:label = "CANCEL"; break;
  }
  wuwPanel(k.x + 1, k.y + 1, k.w - 2, k.h - 3, lit);
  uint16_t col = (k.kind == K_OK) ? rgb565(SK.accent)
               : (k.kind == K_CANCEL) ? rgb565(SK.rec) : rgb565(SK.text);
  if (k.kind == K_SHOW && !kb.mask) col = rgb565(SK.shadow);     // nothing to reveal
  if (label) kbLabelOn(k, big, label, col);
}

static void kbDrawField() {
  wuwPanel(6, KB_FIELD_Y, PW - 12, 24, true);
  char shown[24];
  int show = kb.len > 20 ? 20 : kb.len;                  // the tail, so typing stays visible
  for (int i = 0; i < show; ++i)
    shown[i] = kb.mask && !kb.reveal ? '*' : kb.text[kb.len - show + i];
  shown[show] = 0;
  lcd.setTextSize(2);
  lcd.setTextDatum(textdatum_t::middle_left);
  lcd.setTextColor(rgb565(SK.text));
  lcd.drawString(shown, 12, KB_FIELD_Y + 12);
  int cx = 12 + show * 12;
  lcd.fillRect(cx, KB_FIELD_Y + 5, 2, 14, rgb565(SK.accent));       // caret
  lcd.setTextDatum(textdatum_t::top_left);
  lcd.setTextSize(1);
}

static void kbDrawTitle() {
  lcd.fillRect(0, OS_BODY_Y, PW, KB_FIELD_Y - OS_BODY_Y - 1, rgb565(SK.bg));
  osText(kb.title, 6, OS_BODY_Y + 4, rgb565(SK.accent));
  char n[12]; snprintf(n, sizeof(n), "%u/%u", (unsigned)kb.len, (unsigned)kb.maxLen);
  osText(n, PW - 6, OS_BODY_Y + 4, inkOn(), textdatum_t::top_right);
}

static void kbDrawAll() {
  lcd.fillRect(0, OS_BODY_Y, PW, PH - OS_BODY_Y, rgb565(SK.bg));
  kbDrawTitle();
  kbDrawField();
  kbBuild();
  for (int i = 0; i < kbN; ++i) kbDrawKey(i, false);
}

static void kbOpen(uint8_t purpose, const char* title, bool mask, uint8_t maxLen) {
  memset(&kb, 0, sizeof(kb));
  kb.active = true; kb.purpose = purpose; kb.mask = mask; kb.maxLen = maxLen;
  kb.pressed = -1;
  snprintf(kb.title, sizeof(kb.title), "%s", title);
  kbDrawAll();
}

static void kbClose(bool ok) {
  uint8_t purpose = kb.purpose;
  char text[66]; memcpy(text, kb.text, sizeof(text));
  kb.active = false; kb.pressed = -1;
  /* The finger that pressed OK or CANCEL is still on the glass. Left alone, the
     next osTouch() would see it as a brand-new press on whatever screen is now
     underneath. Marking the press as already in progress makes its release a
     no-op. */
  osDown = true; osHomeTouch = false; osHeld = -1;
  osRepaint = 1;
  if (!ok) { wifiPendingSsid[0] = 0; return; }
  if (purpose == KB_NAME) {
    gateSay(bwoGameSetName(text) ? "NAME SAVED" : "NAME NOT SAVED");
    return;
  }
  if (purpose == KB_SSID) {                         // hidden network: now ask for its password
    snprintf(wifiPendingSsid, sizeof(wifiPendingSsid), "%.32s", text);
    char t[44]; snprintf(t, sizeof(t), "PASSWORD FOR %.28s", wifiPendingSsid);
    kbOpen(KB_PASS, t, true, 63);
    osDown = true;
    return;
  }
  if (purpose == KB_PASS) {
    if (wifiApi.join && wifiPendingSsid[0]) wifiApi.join(wifiPendingSsid, text);
    memset(text, 0, sizeof(text));
  }
}

/* The pilgrim's name, edited on the same keyboard. It opens from a tap on the WORLD STYLE page,
   so that finger is still down: kb.fingerDown makes its release a no-op instead of a key press. */
static void bwoNameEdit() {
  kbOpen(KB_NAME, "PILGRIM NAME", false, BWO_NAME_MAX);
  snprintf(kb.text, sizeof(kb.text), "%s", bwoGameName());
  kb.len = (uint8_t)strlen(kb.text);
  kbDrawField(); kbDrawTitle();
  kb.fingerDown = true;
}

static int kbHit(int x, int y) {
  for (int i = 0; i < kbN; ++i) {
    const KbKey& k = kbKeys[i];
    if (x >= k.x - 1 && x < k.x + k.w + 1 && y >= k.y - 1 && y < k.y + k.h)
      return i;
  }
  return -1;
}

static void kbPress(int i) {
  const KbKey& k = kbKeys[i];
  switch (k.kind) {
    case K_CHAR: case K_SPACE: {
      char c = kb.purpose == KB_NAME ? bwoNameUpper(k.ch) : k.ch;   // names are upper case, a few marks only
      if (kb.purpose == KB_NAME && !bwoNameCharOk(c)) break;
      if (kb.len < kb.maxLen) {
        kb.text[kb.len++] = c; kb.text[kb.len] = 0;
        kbDrawField(); kbDrawTitle();
      }
      if (kb.layer == 1) { kb.layer = 0; kbBuild(); for (int j = 0; j < kbN; ++j) kbDrawKey(j, false); }
      break;
    }
    case K_BKSP:
      if (kb.len) { kb.text[--kb.len] = 0; kbDrawField(); kbDrawTitle(); }
      break;
    case K_SHIFT:
      kb.layer = kb.layer == 1 ? 0 : 1; kbBuild();
      for (int j = 0; j < kbN; ++j) kbDrawKey(j, false);
      break;
    case K_SYM:                                   // abc -> 123 ; on symbols toggles 123 <-> #+=
      kb.layer = kb.layer == 2 ? 3 : kb.layer == 3 ? 2 : 2; kbBuild();
      for (int j = 0; j < kbN; ++j) kbDrawKey(j, false);
      break;
    case K_ABC:
      kb.layer = 0; kbBuild();
      for (int j = 0; j < kbN; ++j) kbDrawKey(j, false);
      break;
    case K_SHOW:
      if (kb.mask) { kb.reveal = !kb.reveal; kbDrawField(); kbDrawKey(i, false); }
      break;
    case K_OK:     if (kb.len || kb.purpose == KB_PASS || kb.purpose == KB_NAME) kbClose(true); break;   // an empty name is the default
    case K_CANCEL: kbClose(false); break;
  }
}

static void kbTouch(bool down, int x, int y) {
  if (down && !kb.fingerDown) {
    kb.fingerDown = true;
    int k = kbHit(x, y);
    kb.pressed = (int8_t)k;
    if (k >= 0) {
      kbDrawKey(k, true);
      kbPress(k);                                  // may close or relayout the keyboard
    }
  } else if (!down && kb.fingerDown) {
    kb.fingerDown = false;
    if (kb.active && kb.pressed >= 0 && kb.pressed < kbN) kbDrawKey(kb.pressed, false);
    kb.pressed = -1;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  WIFI
 *  Scan, pick a network, type the password, watch it join. The access point
 *  stays up throughout; this is only the upstream link.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define WIFI_ROWS      4
#define WIFI_ROW_Y     (OS_BODY_Y + 50)
#define WIFI_ROW_H     28
static PanelWifiNet   wifiNets[12];
static int            wifiNetN = 0, wifiPage = 0;
static bool           wifiScanning = false;
static PanelWifiState wifiSt = {};
static uint32_t       wifiPollAt = 0, wifiForgetArm = 0;
static bool           wifiJoining = false;          // a join was started from this screen

static void wifiStartScan() {
  if (!wifiApi.scanStart) return;
  wifiNetN = 0; wifiPage = 0; wifiScanning = true;
  wifiApi.scanStart();
  osRepaint = 1;
}

static void wifiEnter() {
  if (wifiApi.state) wifiApi.state(&wifiSt);
  wifiJoining = false;
  wifiStartScan();
}

/* Called every pass of the panel loop while the WiFi screen is up. Cheap: it
   only talks to the radio twice a second, and only repaints on a real change. */
static void wifiService() {
  if (osScreen != S_WIFI || kb.active || millis() - wifiPollAt < 500) return;
  wifiPollAt = millis();
  if (wifiScanning && wifiApi.scanResults) {
    int n = wifiApi.scanResults(wifiNets, 12);
    if (n >= 0) { wifiScanning = false; wifiNetN = n; osRepaint = 1; }
  }
  if (wifiApi.state) {
    PanelWifiState now; wifiApi.state(&now);
    bool changed = now.connected != wifiSt.connected || now.connecting != wifiSt.connecting ||
                   now.reason != wifiSt.reason || strcmp(now.ssid, wifiSt.ssid) != 0 ||
                   (now.connected && abs(now.rssi - wifiSt.rssi) >= 6);
    wifiSt = now;
    if (changed) osRepaint = 1;
  }
}

static void wifiBars(int x, int y, int rssi, uint16_t col, uint16_t dim) {
  int lvl = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  for (int b = 0; b < 4; ++b) {
    int h = 3 + b * 3;
    lcd.fillRect(x + b * 5, y + 12 - h, 3, h, b < lvl ? col : dim);
  }
}

static void osWifi() {
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.35f);
  // ── status header ──
  wuwPanel(4, OS_BODY_Y + 4, PW - 8, 40, false);
  osIcon("wifi", 12, OS_BODY_Y + 11, wifiSt.connected ? rgb565(SK.accent) : inkOn(), 24);
  const char* head; uint16_t hc = rgb565(SK.text); char sub[64] = "";
  if (wifiSt.connected) {
    head = "CONNECTED"; hc = rgb565(SK.accent);
    snprintf(sub, sizeof(sub), "%.20s   %s", wifiSt.ssid, wifiSt.ip);
  } else if (wifiSt.connecting) {
    head = "JOINING..."; snprintf(sub, sizeof(sub), "%.28s", wifiSt.ssid);
  } else if (wifiSt.reason == 1) {
    head = "WRONG PASSWORD"; hc = rgb565(SK.rec);
    snprintf(sub, sizeof(sub), "%.20s - tap it and retype", wifiSt.ssid);
  } else if (wifiSt.reason == 2) {
    head = "NETWORK NOT FOUND"; hc = rgb565(SK.rec);
    snprintf(sub, sizeof(sub), "%.20s - move closer, SCAN", wifiSt.ssid);
  } else if (wifiSt.configured && wifiSt.reason) {
    head = "COULD NOT CONNECT"; hc = rgb565(SK.rec);
    snprintf(sub, sizeof(sub), "%.28s", wifiSt.ssid);
  } else {
    head = "NOT CONNECTED"; snprintf(sub, sizeof(sub), "tap a network below");
  }
  osText(head, 44, OS_BODY_Y + 11, hc);
  osText(sub, 44, OS_BODY_Y + 26, inkOn());
  if (wifiSt.connected) {
    char r[12]; snprintf(r, sizeof(r), "%d dBm", (int)wifiSt.rssi);
    osText(r, PW - 12, OS_BODY_Y + 11, inkOn(), textdatum_t::top_right);
  }
  // ── network list ──
  if (wifiScanning) {
    osText("SCANNING...", PW / 2, WIFI_ROW_Y + 40, rgb565(SK.accent), textdatum_t::top_center);
    return;
  }
  if (!wifiNetN) {
    osText("NO NETWORKS FOUND", PW / 2, WIFI_ROW_Y + 32, rgb565(SK.text), textdatum_t::top_center);
    osText("press SCAN, or OTHER to type a hidden name", PW / 2, WIFI_ROW_Y + 48,
           inkOn(), textdatum_t::top_center);
    return;
  }
  int pages = (wifiNetN + WIFI_ROWS - 1) / WIFI_ROWS;
  if (wifiPage >= pages) wifiPage = 0;
  for (int r = 0; r < WIFI_ROWS; ++r) {
    int i = wifiPage * WIFI_ROWS + r;
    if (i >= wifiNetN) break;
    int y = WIFI_ROW_Y + r * WIFI_ROW_H;
    bool cur = wifiSt.connected && !strcmp(wifiNets[i].ssid, wifiSt.ssid);
    wuwPanel(6, y, PW - 12, WIFI_ROW_H - 3, cur);
    wifiBars(14, y + 6, wifiNets[i].rssi, cur ? rgb565(SK.accent) : rgb565(SK.text), inkOn());
    char nm[30]; snprintf(nm, sizeof(nm), "%.24s", wifiNets[i].ssid);
    osText(nm, 42, y + 9, cur ? rgb565(SK.accent) : rgb565(SK.text));
    osText(cur ? "CONNECTED" : wifiNets[i].secure ? "PASSWORD" : "OPEN",
           PW - 14, y + 9, inkOn(), textdatum_t::top_right);
  }
  char p[32]; snprintf(p, sizeof(p), "PAGE %d/%d", wifiPage + 1, pages);
  osText(p, PW / 2, OS_BODY_Y + OS_BODY_H - 11, inkOn(), textdatum_t::top_center);
}

static void wifiPick(int row) {
  int i = wifiPage * WIFI_ROWS + row;
  if (row < 0 || row >= WIFI_ROWS || i >= wifiNetN) return;
  snprintf(wifiPendingSsid, sizeof(wifiPendingSsid), "%s", wifiNets[i].ssid);
  if (!wifiNets[i].secure) {                              // open network: nothing to type
    if (wifiApi.join) wifiApi.join(wifiPendingSsid, "");
    osRepaint = 1;
    return;
  }
  char t[44]; snprintf(t, sizeof(t), "PASSWORD FOR %.28s", wifiPendingSsid);
  kbOpen(KB_PASS, t, true, 63);
}

static void wifiBodyTap(int x, int y) {
  (void)x;
  if (wifiScanning || y < WIFI_ROW_Y) return;
  wifiPick((y - WIFI_ROW_Y) / WIFI_ROW_H);
}

static void wifiKey(int i) {
  switch (i) {
    case 0: osGo(S_MORE); break;
    case 1: wifiStartScan(); break;
    case 2: if (wifiNetN > WIFI_ROWS) { wifiPage = (wifiPage + 1) % ((wifiNetN + WIFI_ROWS - 1) / WIFI_ROWS); osRepaint = 1; } break;
    case 3: kbOpen(KB_SSID, "NETWORK NAME (HIDDEN NETWORK)", false, 32); break;
    case 4:                                                // forget, with a second tap to confirm
      if (!wifiSt.configured) { gateSay("NOTHING SAVED"); osRepaint = 1; }
      else if ((int32_t)(wifiForgetArm - millis()) > 0) {
        if (wifiApi.forget) wifiApi.forget();
        wifiForgetArm = 0; wifiSt.configured = false; wifiSt.connected = false;
        wifiSt.reason = 0; wifiSt.ssid[0] = 0; osRepaint = 1;
      } else { wifiForgetArm = millis() + 3000; gateSay("TAP FORGET AGAIN", 3000); osRepaint = 1; }
      break;
  }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  LIGHT: ISO and exposure
 *  A live-camera screen like the rituals, so the picture changes as you step.
 *  Stops are INDICES; the camera side knows what each one means on the sensor
 *  actually fitted. ISO labels are gain-ratio equivalents, and exposure steps
 *  are relative (the sensor reports no absolute shutter time) -- the screen
 *  says so rather than pretending to be a light meter.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void lightLabel(char* out, size_t n, bool iso) { lightName(iso, iso ? osIso : osExp, out, n); }

static void lightApply(bool saveIt) {
  if (fnCameraFwd) {
    fnCameraFwd(PANEL_CAM_ISO, osIso);
    fnCameraFwd(PANEL_CAM_EXPOSURE, osExp);
  }
  if (saveIt) {
    Preferences lp; lp.begin("wuwlight", false);
    lp.putUChar("iso", osIso); lp.putUChar("exp", osExp); lp.end();
  }
}

static void lightLoad() {
  Preferences lp; lp.begin("wuwlight", true);
  osIso = lp.getUChar("iso", 0); osExp = lp.getUChar("exp", 0); lp.end();
  if (osIso >= PANEL_ISO_STOPS) osIso = 0;
  if (osExp >= PANEL_EXP_STOPS) osExp = 0;
  if (osIso || osExp) lightApply(false);               // restore a manual look after a reboot
}

static void lightHud() {
  char iso[10], ex[10]; lightLabel(iso, sizeof(iso), true); lightLabel(ex, sizeof(ex), false);
  uint32_t sig = hudHashInt(2166136261u, osIso);
  sig = hudHashInt(sig, osExp); sig = hudHashInt(sig, (uint32_t)(int)pstat.aeLevel + 8);
  static uint32_t lastSig = 0;
  if (!hudForce && sig == lastSig) return;
  lastSig = sig; hudForce = false;

  wuwPanel(4, OS_BODY_Y + 4, PW - 8, 35, true);
  osIcon("sun", 12, OS_BODY_Y + 12, rgb565(SK.accent), 20);
  char t[44]; snprintf(t, sizeof(t), "ISO %s    SHUTTER %s", iso, ex);
  osText(t, 43, OS_BODY_Y + 8, rgb565(SK.text));
  const char* mode = (osIso && osExp) ? "FULL MANUAL" :
                     osIso ? "ISO FIXED, SHUTTER AUTO" :
                     osExp ? "SHUTTER FIXED, ISO AUTO" : "CAMERA CHOOSES BOTH";
  osText(mode, 43, OS_BODY_Y + 23, rgb565(SK.accent));

  // where you are on each scale: a dot per stop
  wuwPanel(4, OS_BODY_Y + OS_BODY_H - 27, PW - 8, 23, true);
  int by = OS_BODY_Y + OS_BODY_H - 18;
  osText("ISO", 10, by - 3, inkOn());
  for (int i = 0; i < PANEL_ISO_STOPS; ++i) {
    if (i == osIso) lcd.fillCircle(40 + i * 15, by + 1, 4, rgb565(SK.accent));
    else            lcd.drawCircle(40 + i * 15, by + 1, 2, inkOn());
  }
  osText("EXP", 150, by - 3, inkOn());
  for (int i = 0; i < PANEL_EXP_STOPS; ++i) {
    if (i == osExp) lcd.fillCircle(182 + i * 12, by + 1, 4, rgb565(SK.accent));
    else            lcd.drawCircle(182 + i * 12, by + 1, 2, inkOn());
  }
  if (gateNotice[0] && (int32_t)(gateNoticeUntil - millis()) <= 0) gateNotice[0] = 0;
}

static void osLight() {
  osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, 0.55f);
  if (!pstat.cam)
    osText("NO CAMERA", PW / 2, OS_BODY_Y + 82, rgb565(SK.rec), textdatum_t::top_center);
  lightHud();
}

static void lightKey(int i) {
  switch (i) {
    case 0: osGo(S_HOME); return;
    case 1: if (osIso > 0) --osIso; break;
    case 2: if (osIso + 1 < PANEL_ISO_STOPS) ++osIso; break;
    case 3: if (osExp > 0) --osExp; break;
    case 4: if (osExp + 1 < PANEL_EXP_STOPS) ++osExp; break;
  }
  lightApply(true);
  if (!pstat.cam) osRepaint = 1;                  // with a picture the next frame redraws the HUD
}

static void osDraw() {
  hudForce = true;                 // the HUDs below gate themselves on this
  osStatus();
  if (kb.active) { kbDrawAll(); return; }   // e.g. a status change mid-typing: keep the keyboard
  switch (osScreen) {
    case S_HOME:   osHomeFrame(); break;
    case S_LOOKS:  osLooks();    break;
    case S_TUNE:   osTune();     break;
    case S_CARD:   osCard();     break;
    case S_MORE:   osList(MORE_ROWS, MORE_N); break;
    case S_PRESET: osPresets();  break;
    case S_ROOM:   osRoom();     break;
    case S_SET:    osSettings(); break;
    case S_CAMERA: osCamera();   break;
    case S_SYS:    osSystem();   break;
    case S_WIFI:   osWifi();     break;
    case S_LIGHT:  osLight();    break;
    case S_GAMES:  osGames(); break;
    case S_BWO_PATCHES: osBwoLibrary(); break;
    case S_BWO_STYLE: osBwoStyle(); break;
    case S_BWO_MANAGE: osBwoManage(); break;
    case S_BWO_PHOTOS: osBwoPhotos(); break;
    case S_COLLECTIVE: osCollective(); break;
    case S_GHOST:      osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, .5f); osGhost(); break;
    case S_APOPHENIA: osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, .5f); osApophenia(); break;
    case S_ORACLE:     osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, .5f); osOracle(); break;
    case S_GAME:
      bwoLoadWalls();
      if (!bwoGameBegin(sdOk)) {
        osInfoPanel();
        osText("GAME MEMORY UNAVAILABLE", PW / 2, OS_BODY_Y + 70,
               rgb565(SK.rec), textdatum_t::top_center);
      } else osBwoFrame(millis());
      break;
    case S_RELAY:    osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, .5f); osRelay(); break;
    case S_EXPOSURE: osGround(OS_BODY_Y, OS_BODY_Y + OS_BODY_H, .5f); osExposure(); break;
    case S_GATES:  osGates();    break;
    default:       osInfoPanel(); break;
  }
  if (osScreen == S_HOME) osHomeHud(true);
  osBar();
}


/* ── touch ───────────────────────────────────────────────────────────────
   Same hit regions and the same resulting state changes as the simulator.
   Key presses act on RELEASE so the sunken key is visible while held; body
   taps act immediately, which is what a slider needs. */
static bool     osBwoToolbarTouch = false;
static int      osDragRow = -1;
static uint8_t  osHomeAxis = 0;  // 0 pending, 1 zoom, 2 exposure, 3 contrast
static int      osHomeStartX = 0, osHomeStartY = 0;
static int      osHomeStartZoom = 100, osHomeStartAe = 0, osHomeStartCt = 0;
static uint32_t osHomeDownAt = 0, osHomeLastTap = 0;

static void osGo(int screen) {
  int previous = osScreen;
  if (previous == S_HOME || screen == S_HOME) stFrames = 0;
  if (osScreen == S_GAME && screen != S_GAME) {
    bwoGameTouch(0, 0, false);
    linkGameStop();
  }
  if (osScreen == S_EXPOSURE && screen != S_EXPOSURE) exposureLeave();
  osScreen = screen;
  if (screen == S_WIFI) wifiEnter();
  if (screen == S_CARD) osCardReturn = previous == S_BWO_PHOTOS ? S_BWO_PHOTOS :
    previous == S_GAME || previous == S_BWO_STYLE ? S_GAME : S_HOME;
  if(screen == S_BWO_MANAGE) bwoResetUntil=0;
  if(screen == S_BWO_PATCHES) {bwoSaveUntil=0;bwoPatchScanned=false;}
  osSel = (screen == S_MORE) ? -1 : 0;
  osPage = 0;
  if (screen == S_CARD && sdOk) {
    galScan();
    galSel = 0;
    galTop = 0;
  }
  if (screen == S_GATES) gateEnter();
  if (screen == S_GHOST) ghostReset();
  if (screen == S_APOPHENIA || screen == S_ORACLE || screen == S_RELAY ||
      screen == S_EXPOSURE) {
    gateMetricsValid = false;
    wuwGatesResetAnalysis();
  }
  if (screen == S_RELAY) relayLoad();
  if (screen == S_EXPOSURE) exposureApply();
  osRepaint = 1;
}

static void osKeyAction(int i) {
  switch (osScreen) {
    case S_HOME:
      if      (i == 0) { if (fnShootFwd) fnShootFwd("panel"); }
      else if (i == 1) {
        bool start = !uiRec;
        uiRec = fnRecFwd ? fnRecFwd(start) : false;
        osRepaint = 1;
      }
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
      if      (i == 0) osGo(osCardReturn);
      else if (i == 1) { if (galSel < galN) {
                           if (galVid[galSel]) galPlayVideo(galSel);
                           else                galShowPhoto(galSel);
                           osRepaint = 1; } }
      else if (i == 2) {
        if (galSel >= galN || galVid[galSel]) {
          snprintf(cardNotice, sizeof(cardNotice), "SELECT A PHOTO");
        } else if (bwoRememberWall(galName[galSel])) {
          bwoLoadWalls();
          if (bwoLastWallSlot >= 0) bwoGameSelectWallTexture((uint8_t)bwoLastWallSlot);
          snprintf(cardNotice, sizeof(cardNotice), "WALL %u/4 ADDED",
                   (unsigned)bwoGameWallTextureCount());
        } else snprintf(cardNotice, sizeof(cardNotice), "WALL FAILED");
        cardNoticeUntil = millis() + 1800; osRepaint = 1;
      }
      else if (i == 3) { galTop = (galTop + 6 < galN) ? galTop + 6 : 0; osRepaint = 1; }
      else if (i == 4) osGo(S_MORE);
      break;
    case S_MORE:
      if (i == 0) osGo(S_HOME);
      break;
    case S_GAMES:
      if      (i == 0) osGo(S_HOME);
      else if (i == 4) osGo(S_MORE);
      break;
    case S_GAME:
      if      (i == 0) osGo(S_BWO_PATCHES);
      else if (i == 1) {
        if (bwoGameBuildMode()) {gateSay(bwoGameRemoveWall()?"BLOCK REMOVED":"NO REMOVABLE BLOCK");osRepaint=1;}
        else osGo(S_BWO_STYLE);
      }
      else if (i == 2) {
        if (bwoGameBuildMode()) {
          bool built = bwoGamePlaceWall();
          if (built) ritualComplete(4);
          gateSay(built ? "WORLD CHANGED" : "NO BUILD SPACE");
          osRepaint = 1;
        } else bwoGameFire();
      }
      else if (i == 3) { bwoGameToggleBuild(); osRepaint = 1; }
      else if (i == 4) {
        osGo(S_BWO_PHOTOS);
      }
      break;
    case S_BWO_PATCHES:
      if(i==0)osGo(S_GAMES);
      else if(i==1) {
        if(bwoPatchSaved[bwoPatchSelection] && (int32_t)(bwoSaveUntil-millis())<=0) {
          bwoSaveUntil=millis()+4000;gateSay("SAVE AGAIN TO REPLACE",4000);
        } else {bool ok=bwoGameSavePatch(bwoPatchSelection);bwoSaveUntil=0;
          if(ok)bwoPatchSaved[bwoPatchSelection]=true;
          gateSay(ok?"PATCH SAVED":"SAVE FAILED / CHECK SD");}
        osRepaint=1;
      } else if(i==2) {
        if (!bwoPatchSaved[bwoPatchSelection]) osGo(S_GAME);
        else if (bwoGameLoadPatch(bwoPatchSelection)) {
          bwoPatchPalette=true; bwoWallsLoaded=true; osGo(S_GAME);
        } else {gateSay("NO VALID PATCH");osRepaint=1;}
      } else if(i==3)osGo(S_BWO_STYLE);
      else if(i==4)osGo(S_BWO_MANAGE);
      break;
    case S_BWO_STYLE:
      if(i==0||i==4)osGo(S_BWO_PATCHES);
      else if(i==2)osGo(S_GAME);
      else if(i==3)osGo(S_CARD);
      break;
    case S_BWO_MANAGE:
      if(i==0)osGo(S_BWO_PATCHES);
      else if(i==4 && (int32_t)(bwoResetUntil-millis())>0) {
        bwoGameReset();bwoPatchPalette=false;bwoWallsLoaded=false;
        bwoResetUntil=0;osGo(S_GAME);
      }
      break;
    case S_BWO_PHOTOS:
      if(i==0)osGo(S_GAME);
      else if(i>=1 && i<=4) {
        if(bwoGameSelectWallTexture(i-1))osGo(S_GAME);
        else {gateSay("ADD A PHOTO FIRST");osRepaint=1;}
      }
      break;
    case S_COLLECTIVE:
      if      (i == 0) osGo(S_HOME);
      else if (i == 1) gateSay("RITUAL REARMED");
      else if (i == 2) {
        bool ok = fnCollectiveFwd && fnCollectiveFwd("collective");
        if (ok) ritualComplete(0);
        gateSay(ok ? "SYNC ARMED: 500MS" : "NO LINKED CAMERAS", 2200);
        osRepaint = 1;
      } else if (i == 3) {
        char b[24]; snprintf(b, sizeof(b), "%u PEERS READY", (unsigned)pstat.peers);
        gateSay(b); osRepaint = 1;
      } else if (i == 4) osGo(S_MORE);
      break;
    case S_GHOST:
      if      (i == 0) osGo(S_HOME);
      else if (i == 1) { ghostReset(); gateSay("RESIDUE CLEARED"); }
      else if (i == 2) {
        int gain = ritualScoreFor(S_GHOST) / 4;
        if (gain < 2) gateSay("FRAME MORE MOTION");
        else { ghostResidue = min(100, (int)ghostResidue + gain); gateSay("RESIDUE COLLECTED"); }
      } else if (i == 3) {
        if (ghostResidue < 25) gateSay("COLLECT MORE RESIDUE");
        else if (saveRitualFrame("GHOST")) {
          ritualComplete(1); gateSay("GHOST SAVED TO CARD", 2200);
        } else gateSay("SAVE FAILED", 2200);
      } else if (i == 4) osGo(S_MORE);
      osRepaint = 1;
      break;
    case S_APOPHENIA:
      if (i == 0) osGo(S_HOME);
      else if (i >= 1 && i <= 3) { apoChoose((uint8_t)(i - 1)); osRepaint = 1; }
      else if (i == 4) {
        if (apoPartCount < 3) gateSay("CHOOSE THREE FORMS");
        else if (saveRitualFrame("APOPHENIA")) {
          ritualComplete(2); gateSay("ENTITY SAVED", 2200); apoPartCount = 0;
        } else gateSay("SAVE FAILED");
        osRepaint = 1;
      }
      break;
    case S_WIFI:  wifiKey(i); break;
    case S_LIGHT: lightKey(i); break;
    case S_ORACLE:
      if      (i == 0) osGo(S_HOME);
      else if (i == 1) { ++oracleStep; osRepaint = 1; }
      else if (i == 2) {
        bool ok = fnShootFwd && fnShootFwd("oracle");
        gateSay(ok ? "ANSWER SAVED" : "SHUTTER REFUSED", 2200); osRepaint = 1;
      } else if (i == 3) {
        bool saved = saveRitualFrame("ORACLE");
        if (saved) ritualComplete(3);
        gateSay(saved ? "ANSWER SAVED" : "SAVE FAILED", 2200);
        osRepaint = 1;
      } else if (i == 4) osGo(S_MORE);
      break;
    case S_RELAY:
      if      (i == 0) osGo(S_HOME);
      else if (i == 1) {
        if (galN) {
          int found = -1;
          for (int n = 0; n < galN; ++n) {
            int candidate = (relayGallery + 1 + n) % galN;
            if (!galVid[candidate]) { found = candidate; break; }
          }
          relayGallery = found;
        }
        osRepaint = 1;
      } else if (i == 2) {
        bool raw = fnShootFwd && fnShootFwd("relay");
        bool art = saveRitualFrame("RELAY");
        if (raw || art) {
          ritualComplete(5);
          ++relayStage; Preferences rp; rp.begin("ritual", false);
          rp.putUShort("relay", relayStage); rp.end();
        }
        gateSay(raw || art ? "RELAY EXTENDED" : "CAPTURE FAILED", 2200);
        osRepaint = 1;
      } else if (i == 3) { uiFx = (uiFx + 1) % FX_COUNT; osRepaint = 1; }
      else if (i == 4) osGo(S_MORE);
      break;
    case S_EXPOSURE:
      if      (i == 0) osGo(S_HOME);
      else if (i == 1 || i == 3) {
        divExposure = constrain(divExposure + (i == 1 ? -100 : 100), 0, 1200);
        exposureApply(); osRepaint = 1;
      } else if (i == 2) {
        int score = ritualScoreFor(S_EXPOSURE);
        if (score < 80) { char b[24]; snprintf(b,sizeof(b),"SIGNAL %d%%",score); gateSay(b); }
        else if (saveRitualFrame("DIVINATION")) {
          ritualComplete(6); divStage = (divStage + 1) & 3;
          gateSay("SYMBOL REVEALED", 2200);
        } else gateSay("SAVE FAILED");
        osRepaint = 1;
      } else if (i == 4) osGo(S_MORE);
      break;
    case S_GATES:
      if      (i == 0) osGo(S_HOME);
      else if (i == 1) { gateResetRequest(); osRepaint = 1; }
      else if (i == 2) { gateScan(); osRepaint = 1; }
      else if (i == 3) osGo(S_BWO_PATCHES);
      else if (i == 4) osGo(S_MORE);
      break;
    default:
      if      (i == 0) osGo(S_HOME);
      else if (i == 4) osGo(S_MORE);
      break;
  }
}

static void osChooseSkin(int index) {
  if (index < 0 || index >= NSKIN || index == uiSkin) return;
  uiSkin = index;
  Preferences pp; pp.begin("wuw", false); pp.putUChar("uiskin", uiSkin); pp.end();
  osRepaint = 1;
}

static bool osCameraSet(PanelCameraControl control, int value) {
  if (!fnCameraFwd || !fnCameraFwd(control, value)) return false;
  switch (control) {
    case PANEL_CAM_STREAM_RES:     pstat.streamRes = value; break;
    case PANEL_CAM_PHOTO_RES:      pstat.photoRes = value; break;
    case PANEL_CAM_STREAM_QUALITY: pstat.streamQuality = value; break;
    case PANEL_CAM_PHOTO_QUALITY:  pstat.photoQuality = value; break;
    case PANEL_CAM_BRIGHTNESS:     pstat.brightness = value; break;
    case PANEL_CAM_HMIRROR:        pstat.hmirror = value != 0; break;
    case PANEL_CAM_VFLIP:          pstat.vflip = value != 0; break;
    default: break;
  }
  osRepaint = 1;
  return true;
}

static void osBodyTap(int x, int y) {
  if (osScreen == S_HOME && x >= HOME_VIEW_X + HOME_VIEW_W - 44 &&
      x < HOME_VIEW_X + HOME_VIEW_W &&
      y >= HOME_VIEW_Y + HOME_VIEW_H - 44 &&
      y < HOME_VIEW_Y + HOME_VIEW_H) {
    osGo(S_CARD);
  } else if (osScreen == S_WIFI) {
    wifiBodyTap(x, y);
  } else if (osScreen == S_HOME && y < HOME_VIEW_Y + 4 && x >= 90 && x < 230) {
    osGo(S_LIGHT);                           // the ISO / EXP readout is also the way in
  } else if(osScreen==S_BWO_PHOTOS) {
    if(y>=OS_BODY_Y+142)osGo(S_CARD);
    else {
      int slot=(y-OS_BODY_Y-4)/34;
      if(slot>=0 && slot<BWO_WALL_TEXTURE_SLOTS) {
        if(bwoGameSelectWallTexture(slot))osGo(S_GAME);
        else {gateSay("ADD A PHOTO FIRST");osRepaint=1;}
      }
    }
  } else if(osScreen==S_BWO_PATCHES) {
    int row=(y-OS_BODY_Y-4)/30;
    if(y>=OS_BODY_Y+130)osGo(S_GAME);
    else if(row>=0&&row<4){bwoPatchSelection=row;bwoSaveUntil=0;osRepaint=1;}
  } else if(osScreen==S_BWO_STYLE) {
    int row=(y-OS_BODY_Y-BWO_STYLE_ROW_Y)/BWO_STYLE_PITCH;
    if(row==0 && !bwoGameCycleTheme())gateSay("COLLECT A FRAGMENT FIRST");
    else if(row==1)bwoGameCycleUv();
    else if(row==2)bwoGameCycleGlitch();
    else if(row==BWO_STYLE_NAME_ROW)bwoNameEdit();
    osRepaint=1;
  } else if(osScreen==S_BWO_MANAGE) {
    if(y>=OS_BODY_Y+92&&y<OS_BODY_Y+130){bwoResetUntil=millis()+5000;osRepaint=1;}
  } else if (osScreen == S_GAME) {
    if (y < OS_BODY_Y + 21) {
      osBwoToolbarTouch = true;
      int tool = constrain(x / (PW / 4), 0, 3);
      if (tool <= 1) {
        if (!bwoGameChangeLevel(tool == 0 ? -1 : 1)) gateSay("LEVEL BLOCKED");
      } else if (tool == 2 && bwoGameBuildMode()) bwoGameCycleFace();
      else if (tool == 3 && bwoGameBuildMode())
        gateSay(bwoGamePaintFace() ? "FACE PAINTED" :
          bwoGameWallTextureCount() ? "AIM AT A WALL" : "ADD PHOTO IN PHOTOS");
      else if (tool == 3) bwoGameToggleBuild();
      osRepaint = 1;
    } else if (x < 160 && y >= OS_BODY_Y + BWO_NAME_Y - 2 && y < OS_BODY_Y + BWO_NAME_Y + BWO_NAME_H + 6) {
      bwoNameEdit();                       // tapping the name tag under the portrait renames the pilgrim
    } else bwoGameTouch(x, y - OS_BODY_Y, true);
  } else if (osScreen == S_EXPOSURE) {
    divGain = constrain(x * 31 / PW, 0, 30);
    exposureApply(); osRepaint = 1;
  } else if (osScreen == S_LOOKS) {
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
  } else if (osScreen == S_GAMES) {
    int rowH = (OS_BODY_H - 6) / GAME_N;
    int i = (y - OS_BODY_Y - 3) / rowH;
    if (i >= 0 && i < GAME_N) osGo(GAME_ROWS[i].go);
  } else if (osScreen == S_SET) {
    const int cols = 4, rows = (NSKIN + cols - 1) / cols;
    int col = x / (PW / cols), row = (y - OS_BODY_Y) / (OS_BODY_H / rows);
    int i = row * cols + col;
    if (i >= 0 && i < NSKIN) osChooseSkin(i);
  } else if (osScreen == S_CAMERA) {
    int row = (y - OS_BODY_Y) / (OS_BODY_H / 7);
    if (row == 0) {
      osCameraSet(PANEL_CAM_STREAM_RES,
                  resNext(pstat.streamRes, false, pstat.maxRes));
    } else if (row == 1) {
      osCameraSet(PANEL_CAM_PHOTO_RES,
                  resNext(pstat.photoRes, true, pstat.maxRes));
    } else if (row == 2) {
      osCameraSet(PANEL_CAM_STREAM_QUALITY,
                  valueNext(LIVE_Q, sizeof(LIVE_Q), pstat.streamQuality));
    } else if (row == 3) {
      osCameraSet(PANEL_CAM_PHOTO_QUALITY,
                  valueNext(PHOTO_Q, sizeof(PHOTO_Q), pstat.photoQuality));
    } else if (row == 4) {
      osCameraSet(PANEL_CAM_BRIGHTNESS,
                  pstat.brightness >= 2 ? -2 : pstat.brightness + 1);
    } else if (row == 5) {
      int orient = ((pstat.hmirror ? 1 : 0) | (pstat.vflip ? 2 : 0) ) + 1;
      orient &= 3;
      osCameraSet(PANEL_CAM_HMIRROR, orient & 1);
      osCameraSet(PANEL_CAM_VFLIP, (orient >> 1) & 1);
    } else if (row == 6) {
      osViewRot = (osViewRot + 1) & 3;
      Preferences pp; pp.begin("wuw", false); pp.putUChar("viewrot", osViewRot); pp.end();
      osRepaint = 1;
    }
  }
}

static void osTouch() {
  if (!lcdOK) return;
  static int32_t lastX = 0, lastY = 0;
  static uint8_t misses = 0;
  int32_t x = lastX, y = lastY;
  bool sampled = panelTouchGet(&x, &y);
  if (sampled) { lastX = x; lastY = y; misses = 0; }
  else if (osDown && misses < 3) { ++misses; sampled = true; }
  bool down = sampled;
  if (kb.active) { kbTouch(down, x, y); return; }          // modal: nothing else sees touches
  if (down && !osDown) {                                   // press
    osDown = true;
    osHomeTouch = osScreen == S_HOME &&
      x >= HOME_VIEW_X && x < HOME_VIEW_X + HOME_VIEW_W &&
      y >= HOME_VIEW_Y && y < HOME_VIEW_Y + HOME_VIEW_H &&
      !(x >= HOME_VIEW_X + HOME_VIEW_W - 44 &&
        y >= HOME_VIEW_Y + HOME_VIEW_H - 44);
    if (osHomeTouch) {
      osHomeAxis = 0;
      osHomeStartX = x; osHomeStartY = y;
      osHomeStartZoom = osZoomPct;
      osHomeStartAe = pstat.aeLevel;
      osHomeStartCt = pstat.contrast;
      osHomeDownAt = millis();
      if (osHomeLastTap && osHomeDownAt - osHomeLastTap < 300) {
        osZoomPct = 100;
        osHomeLastTap = 0;
      }
    }
    if (y < OS_BODY_Y && x < 104) {
      osGo(S_HOME);                                        // WUW / VIEW status key
    } else if (y >= OS_BAR_Y) {
      osHeld = x / BTN_W; if (osHeld >= NBTN) osHeld = NBTN - 1;
      osRepaint = 1;
    } else if (y >= OS_BODY_Y) {
      osBodyTap(x, y);
    }
  } else if (down && osDown && osHomeTouch && osScreen == S_HOME) {
    int dx = x - osHomeStartX, dy = y - osHomeStartY;
    if (!osHomeAxis && abs(dx) >= 12 && abs(dx) > abs(dy) * 5 / 4)
      osHomeAxis = 1;
    if (!osHomeAxis && millis() - osHomeDownAt >= 180 &&
        abs(dy) >= 12 && abs(dy) > abs(dx) * 5 / 4)
      osHomeAxis = osHomeStartX < HOME_VIEW_X + HOME_VIEW_W / 2 ? 2 : 3;
    if (osHomeAxis == 1) {
      osZoomPct = constrain(osHomeStartZoom + dx * 2, 100, 300);
    } else if (osHomeAxis == 2) {
      int level = constrain(osHomeStartAe - dy / 24, -2, 2);
      if (level != pstat.aeLevel && fnCameraFwd &&
          fnCameraFwd(PANEL_CAM_AE_LEVEL, level)) pstat.aeLevel = level;
    } else if (osHomeAxis == 3) {
      int level = constrain(osHomeStartCt - dy / 24, -2, 2);
      if (level != pstat.contrast && fnCameraFwd &&
          fnCameraFwd(PANEL_CAM_CONTRAST, level)) pstat.contrast = level;
    }
  } else if (down && osDown && osDragRow >= 0 && osScreen == S_TUNE) {
    int yy, h, tx, tw, ty; osTuneGeom(osDragRow, &yy, &h, &tx, &tw, &ty);
    float v = (float)(x - tx - 2) / (float)(tw - 4);
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    osP[osDragRow] = (osDragRow == 5) ? 1.0f + v * 2.0f : v;
    osRepaint = 1;
  } else if (down && osDown && osScreen == S_GAME &&
             lastY < OS_BAR_Y && !osBwoToolbarTouch) {
    bwoGameTouch(lastX, lastY - OS_BODY_Y, true);
  } else if (!down && osDown) {                            // release
    if (osScreen == S_GAME) bwoGameTouch(0, 0, false);
    if (osHomeTouch && !osHomeAxis && millis() - osHomeDownAt < 250)
      osHomeLastTap = millis();
    osDown = false; osDragRow = -1; osBwoToolbarTouch = false;
    osHomeTouch = false; osHomeAxis = 0;
    if (osHeld >= 0) { int k = osHeld; osHeld = -1; osRepaint = 1; osKeyAction(k); }
  }
}


/* ── the bridge to the rest of the firmware ─────────────────────────────── */
static volatile bool   panelUp = false;
static TaskHandle_t    panelTaskH = nullptr;

void panelTileConfig(int on, int thresh) {
  (void)thresh;  // old threshold is ignored: exact comparison cannot go stale
  if (on >= 0)     tileOn = (on != 0);
  stTiles = stTotal = stUs = stFrames = 0;      // stats describe one setting
}

size_t panelTileStats(char* out, size_t cap) {
  if (!out || cap < 160) return 0;
  double pct = stTotal ? (100.0 * stTiles / stTotal) : 0.0;
  double ms  = stFrames ? (stUs / 1000.0 / stFrames) : 0.0;
  return (size_t)snprintf(out, cap,
    "{\"on\":%s,\"thresh\":%u,\"frames\":%u,\"tilePct\":%.1f,\"pushMs\":%.1f,\"fps\":%.1f,\"viewPushMs\":%.1f}",
    tileOn ? "true" : "false", 0u,
    (unsigned)stFrames, pct, ms, viewFps, viewPushMs);
}

void panelPublish(const PanelStatus& s) {
  if(pstat.sd != s.sd) bwoPatchScanned=false;
  bool changed = pstat.cam != s.cam || pstat.sd != s.sd || pstat.net != s.net ||
                 pstat.peers != s.peers ||
                 pstat.visitors != s.visitors || pstat.photos != s.photos ||
                 pstat.streamRes != s.streamRes || pstat.photoRes != s.photoRes ||
                 pstat.streamQuality != s.streamQuality ||
                 pstat.photoQuality != s.photoQuality ||
                 pstat.brightness != s.brightness ||
                 pstat.hmirror != s.hmirror || pstat.vflip != s.vflip;
  pstat = s;
  if (changed) osRepaint = 1;
}
void panelHooks(PanelShootFn a, PanelRecFn b, PanelCameraFn d, PanelCollectiveFn e,
                PanelLabelFn l) {
  fnLabelFwd = l;
  fnShootFwd = a;
  fnRecFwd = b;
  fnCameraFwd = d;
  fnCollectiveFwd = e;
}
void panelWifiHooks(const PanelWifiApi& api) { wifiApi = api; }
bool panelPresent() { return panelUp; }
bool panelTouchReady() { return touchCalValid(); }
bool panelSetBwoWall(const char* path) {
  bool ok = bwoRememberWall(path);
  if (ok) {
    bwoLoadWalls();
    if (bwoLastWallSlot >= 0) bwoGameSelectWallTexture((uint8_t)bwoLastWallSlot);
    osRepaint = 1;
  }
  return ok;
}
void panelSetViewRot(uint8_t quarterTurnsClockwise) {
  osViewRot = quarterTurnsClockwise & 3;
  osRepaint = 1;
}
bool panelSetSkin(const char* name) {
  if (!name || !*name) return false;
  for (int i = 0; i < NSKIN; ++i) {
    if (!strcasecmp(name, SKINS[i].name)) { osChooseSkin(i); return true; }
  }
  return false;
}

/* Owns the panel end to end. Core 1 keeps camera decode and LCD traffic away
   from the WiFi/TCP stack, which Arduino-ESP32 predominantly runs on core 0. */
static void panelTaskFn(void*) {
  uint32_t lastFrame = 0;
  for (;;) {
    camOK = pstat.cam;                 // the OS reads these when it draws
    sdOk  = pstat.sd;
    osTouch();
    if (osRepaint) {
      osRepaint = 0;
      osDraw();
      if (osScreen == S_HOME) stFrames = 0;
    }

    wifiService();                     // polls the radio twice a second, only on the WiFi screen

    /* Camera rituals share one bounded decode path. Menus, collective sync,
       and BWO leave the sensor untouched. LIGHT rides the same path with no
       effect and no scene analysis: it only needs the picture. */
    bool ritualCamera = osScreen == S_GATES || osScreen == S_GHOST ||
                        osScreen == S_APOPHENIA || osScreen == S_ORACLE ||
                        osScreen == S_RELAY || osScreen == S_EXPOSURE ||
                        osScreen == S_LIGHT;
    bool cameraScreen = osScreen == S_HOME || ritualCamera;
    uint32_t cameraPeriod = ritualCamera ? 85 : 33;
    /* While a clip is being recorded the screen is the lowest priority: every
       decode, every colour effect and every push is CPU, PSRAM-bandwidth and SPI
       time the capture and SD paths could use, and a smooth recording matters
       more than a 30 fps preview. So the preview drops to about 10 fps and is
       drawn plain (no panel effect). */
    const bool recording = rec_is_active();
    if (recording && cameraPeriod < 100) cameraPeriod = 100;
    if (cameraScreen && pstat.cam && !kb.active && (millis() - lastFrame) > cameraPeriod) {
      lastFrame = millis();
      /* Not esp_camera_fb_get(): a frame off the sensor goes to ONE caller, and
         during a recording that caller has to be the recorder. camFrameAcquire
         hands back a real frame normally and the recorder's latest otherwise. */
      CamFrame cf;
      static uint32_t lastSharedSeq = 0;
      bool got = camFrameAcquire(&cf);
      if (got && cf.slot >= 0 && cf.seq == lastSharedSeq) {   // already showed this one
        camFrameRelease(&cf);
        got = false;
      }
      if (got) {
        if (cf.slot >= 0) lastSharedSeq = cf.seq;
        if (viewBuf) {
          int w = 0, h = 0;
          if (decodeInto(const_cast<uint8_t*>(cf.buf), cf.len, viewBuf, PW, PH, &w, &h)) {
            if (ritualCamera) {
              if (osScreen != S_LIGHT) {
                gateMetrics = wuwGatesAnalyze(viewBuf, w, h, PW);
                gateMetricsValid = true;
                ritualFrameW = w; ritualFrameH = h;
              }
              if (osScreen == S_GHOST) ghostFrame(viewBuf, w, h, PW);
              else if (osScreen == S_APOPHENIA) apopheniaFrame(viewBuf, w, h, PW);
              else if ((osScreen == S_GATES || osScreen == S_RELAY ||
                        osScreen == S_ORACLE) && !recording) applyPanelFx(viewBuf, w, h, PW);
              pushGateCamera(w, h);
            } else {
              if (!recording) applyPanelFx(viewBuf, w, h, PW);
              pushHomeCamera(w, h);
            }
          }
        }
        camFrameRelease(&cf);
        if      (osScreen == S_GATES)      osGateHud();
        else if (osScreen == S_GHOST)      osGhost();
        else if (osScreen == S_APOPHENIA) osApophenia();
        else if (osScreen == S_ORACLE)     osOracle();
        else if (osScreen == S_RELAY)      osRelay();
        else if (osScreen == S_EXPOSURE)   osExposure();
        else if (osScreen == S_LIGHT)      lightHud();
        else                               osHomeHud();
      }
    } else if (osScreen == S_GAME && bwoGameReady() &&
               (millis() - lastFrame) >= (recording ? 120u : 50u)) {
      lastFrame = millis();
      osBwoFrame(lastFrame);
    }
    vTaskDelay(pdMS_TO_TICKS(cameraScreen ? 5 : 16));
  }
}

/* The touch controller shares the display's SPI wires but has its own CS.
   T_IRQ is not connected, so Z1 is the sole contact signal. */
#define XPT_X   0xD1
#define XPT_Y   0x91
#define XPT_Z1  0xB1
#define XPT_Z2  0xC1
#define XPT_TOUCH_Z 80      /* diagnostic: idle 0, valid light contact >180 */
#define CALIB_MIN_Z 160     /* resistive corners measure lighter than center */

/* LovyanGFX's touch driver batches channels under one CS assertion. This
   measured HR2046 clone only returns a valid first conversion after CS falls,
   so each X, Y, and Z1 conversion below gets a separate CS window. */

static uint16_t med5(uint16_t* values) {
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
}

/* ── One channel, one CS window ───────────────────────────────────────────
 * The thing that cost most of an evening. On this HR2046 -- a clone of the
 * XPT2046, and evidently not a perfect one -- ONLY THE FIRST CONVERSION AFTER
 * CS GOES LOW IS VALID. Ask for a second channel without raising CS and it
 * answers a constant 108, convincingly and forever.
 *
 * Measured, dragging a finger across the glass:
 *
 *     X  solo, first    0..3742  span 3742      X  in-sequence  463..4095
 *     Y  solo, first  494..4095  span 3601      Y  in-sequence   108..110
 *     Z1 solo, first    0..2255  span 2255      Z1 in-sequence  700..1023
 *
 * The in-sequence column is the same three channels read back to back: the
 * first works and the rest do not. That is why pressure appeared to work
 * while position looked dead -- Z1 happened to be first in my original read
 * order, so the one channel I could see was the one channel that was valid.
 *
 * Every real driver batches these reads into one transaction, which is why
 * LovyanGFX cannot drive this part either. The cost of not batching is one
 * extra CS edge per channel, which is nothing.
 */
static uint16_t xptRead(uint8_t cmd) {
  lcd.waitDMA();
  digitalWrite(LCD_CS, HIGH);
  digitalWrite(TCH_CS, LOW);
  SPI.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));
  delayMicroseconds(20);
  SPI.transfer(cmd);
  uint16_t v = (uint16_t)SPI.transfer(0x00) << 8;
  v |= SPI.transfer(0x00);
  SPI.endTransaction();
  digitalWrite(TCH_CS, HIGH);
  delayMicroseconds(60);
  return (v >> 3) & 0x0FFF;
}

/* One debounced sample, matching the known-good standalone diagnostic. Every
   conversion gets its own CS window. Five readings reject transition noise;
   the coordinate bounds reject this board's idle signature (X=4095). */
static bool touchSample(int* rawX, int* rawY, int* pressure) {
  uint16_t zs[5], xs[5], ys[5], z2s[5];
  for (int i = 0; i < 5; ++i) {
    zs[i] = xptRead(XPT_Z1);
    xs[i] = xptRead(XPT_X);
    ys[i] = xptRead(XPT_Y);
    z2s[i] = xptRead(XPT_Z2);
  }
  uint16_t z1 = med5(zs);
  uint16_t x = med5(xs);
  uint16_t y = med5(ys);
  (void)med5(z2s); // clock Z2 exactly as in the verified diagnostic sequence
  *pressure = z1;
  *rawX = x;
  *rawY = y;
  return z1 >= XPT_TOUCH_Z && x > 100 && x < 4000 && y > 100 && y < 4000;
}

/* Versioned affine calibration. It handles axis swap, reversal, endpoint
   offset and the small skew present in real resistive overlays. The magic
   invalidates the old min/max structure without interpreting stale NVS bytes. */
static constexpr uint32_t TOUCH_CAL_MAGIC = 0x57554334; // "WUC4"
struct TouchCal {
  uint32_t magic;
  float x[3];
  float y[3];
};
static TouchCal tcal = {};

static bool touchCalValid() {
  if (tcal.magic != TOUCH_CAL_MAGIC) return false;
  for (int i = 0; i < 3; ++i)
    if (!isfinite(tcal.x[i]) || !isfinite(tcal.y[i])) return false;
  return true;
}

static void touchCalLoad() {
  Preferences pp; pp.begin("wuw", true);
  size_t n = pp.getBytes("tcal4", &tcal, sizeof(tcal));
  pp.end();
  if (n != sizeof(tcal) || !touchCalValid()) tcal = {};
  printf("[TOUCH] calibration %s\n",
         touchCalValid() ? "loaded" : "ABSENT - run /panel?k=wuw01&calib=1");
}

static void touchCalSave() {
  Preferences pp; pp.begin("wuw", false);
  pp.putBytes("tcal4", &tcal, sizeof(tcal));
  pp.end();
}

/* The panel OS asks through here instead of lcd.getTouch(). */
static bool panelTouchGet(int32_t* outX, int32_t* outY) {
  int rx, ry, z;
  if (!touchSample(&rx, &ry, &z)) return false;
  if (!touchCalValid()) return false;
  int W = lcd.width(), H = lcd.height();
  int x = lroundf(tcal.x[0] * rx + tcal.x[1] * ry + tcal.x[2]);
  int y = lroundf(tcal.y[0] * rx + tcal.y[1] * ry + tcal.y[2]);
  *outX = (int32_t)(x < 0 ? 0 : (x >= W ? W - 1 : x));
  *outY = (int32_t)(y < 0 ? 0 : (y >= H ? H - 1 : y));
  return true;
}

static bool touchSolve3(float m[3][4], float out[3]) {
  for (int col = 0; col < 3; ++col) {
    int pivot = col;
    for (int row = col + 1; row < 3; ++row)
      if (fabsf(m[row][col]) > fabsf(m[pivot][col])) pivot = row;
    if (fabsf(m[pivot][col]) < 0.000001f) return false;
    if (pivot != col) {
      for (int j = col; j < 4; ++j) {
        float v = m[col][j]; m[col][j] = m[pivot][j]; m[pivot][j] = v;
      }
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

/* Three non-collinear points are sufficient for an affine transform. Keeping
   calibration to three short taps matters more on a pocket camera than an
   extra validation target; raw edge-span checks still reject wrong points. */
static bool touchCalibrate() {
  /* Build a candidate separately. If a recalibration attempt is interrupted,
     the last working map stays active rather than leaving touch disabled. */
  TouchCal candidate = {};
  struct Pt { int sx, sy; const char* label; int rx, ry; };
  int W = lcd.width(), H = lcd.height();
  Pt pts[3] = { { 24, 24, "TOP LEFT", 0, 0 },
                { W - 24, 24, "TOP RIGHT", 0, 0 },
                { 24, H - 24, "BOTTOM LEFT", 0, 0 } };
  for (int i = 0; i < 3; i++) {
    lcd.fillScreen(0x0000);
    lcd.setTextColor(0xFFFF, 0x0000); lcd.setTextSize(2);
    lcd.setCursor(W / 2 - 96, H / 2 - 30);
    lcd.printf("marker %d of 3", i + 1);
    lcd.setCursor(W / 2 - 96, H / 2 - 8);
    lcd.print("tap FIRMLY, then lift");
    lcd.setCursor(W / 2 - 90, H / 2 + 4);
    lcd.printf("%-14s", pts[i].label);
    lcd.drawCircle(pts[i].sx, pts[i].sy, 12, 0xFFE0);
    lcd.drawLine(pts[i].sx - 16, pts[i].sy, pts[i].sx + 16, pts[i].sy, 0xFFE0);
    lcd.drawLine(pts[i].sx, pts[i].sy - 16, pts[i].sx, pts[i].sy + 16, 0xFFE0);

    /* Wait for a steady press, then for release, so one long touch cannot
       satisfy two targets. */
    long ax = 0, ay = 0; int n = 0;
    int ticks = 0, miss = 0;
    const uint32_t pressDeadline = millis() + 10000;
    while ((int32_t)(millis() - pressDeadline) < 0) {
      int rx, ry, z;
      /* Calibration requires more pressure than ordinary taps. On this glass
         Z1 is 0 at rest and valid held points measured from about 800 upward;
         lower samples are too sensitive to fingertip movement. */
      bool hit = touchSample(&rx, &ry, &z) && z >= CALIB_MIN_Z;
      /* Live feedback, on the glass and on the wire. Without it a timeout is
         indistinguishable between "nobody touched", "the threshold is wrong"
         and "the reader is broken", and the last two rounds of this were lost
         to exactly that ambiguity. Drawing happens only between samples, and
         the next SPI transaction restores the touch bus configuration before
         it samples, so drawing cannot poison the reported values. */
      if ((ticks % 8) == 0) {
        printf("[CALIB] %-12s z=%4d raw=%4d,%4d %s\n", pts[i].label, z, rx, ry,
               hit ? "CONTACT" : "");
        lcd.setTextSize(2);
        lcd.setTextColor(hit ? 0x07E0 : 0x7BEF, 0x0000);
        lcd.setCursor(6, H - 20);
        lcd.printf("z%4d  %4d,%4d  %s  ", z, rx, ry,
                   hit ? "OK  " : (z >= XPT_TOUCH_Z ? "HARDER" : "...   "));
      }
      ticks++;
      /* Tolerate the odd dropped sample. A resistive panel under a fingertip
         reads intermittently -- the live log shows contact, gap, contact --
         so a single miss must not discard the whole run, or a light touch can
         never satisfy the requirement at all. Three in a row is a real lift. */
      if (hit) { ax += rx; ay += ry; miss = 0; if (++n >= 5) break; }
      else if (++miss >= 3) { n = 0; ax = ay = 0; }
      delay(15);
    }
    if (n < 5) { printf("[CALIB] TIMED OUT waiting for %s\n", pts[i].label); return false; }
    pts[i].rx = (int)(ax / n); pts[i].ry = (int)(ay / n);
    printf("[CALIB] %-13s raw %4d,%4d\n", pts[i].label, pts[i].rx, pts[i].ry);
    lcd.fillCircle(pts[i].sx, pts[i].sy, 8, 0x07E0);
    /* Wait for a REAL lift before offering the next target. The three second
       bound was reached with a finger still resting on the glass, so the next
       target captured the same point -- and four identical corners produce a
       map that is correctly refused as degenerate. It looked like touch not
       working; it was calibration measuring one point four times.
 
       Sustained release, not a single clear sample: the panel drops out
       intermittently under a steady finger, so one no-contact reading proves
       nothing. Still bounded -- nothing on a boot path waits forever. */
    lcd.setTextColor(0xFFE0, 0x0000); lcd.setTextSize(2);
    lcd.setCursor(6, H - 20); lcd.print("lift your finger      ");
    int clear = 0;
    uint32_t rel = millis() + 2500;
    while (millis() < rel && clear < 5) {
      int rx, ry, z;
      clear = touchSample(&rx, &ry, &z) ? 0 : clear + 1;
      delay(20);
    }
    if (clear < 5) {
      printf("[CALIB] cancelled: finger was not lifted after %s\n", pts[i].label);
      lcd.fillScreen(0x0000);
      lcd.setTextColor(0xF800, 0x0000); lcd.setTextSize(2);
      lcd.setCursor(18, H / 2 - 18); lcd.print("LIFT TIMEOUT");
      lcd.setCursor(18, H / 2 + 8); lcd.print("opening camera OS");
      delay(700);
      return false;
    }
    delay(120);
  }
  auto distance2 = [](const Pt& a, const Pt& b) {
    int32_t dx = a.rx - b.rx, dy = a.ry - b.ry;
    return dx * dx + dy * dy;
  };
  const int32_t minEdge2 = 900 * 900;
  if (distance2(pts[0], pts[1]) < minEdge2 ||
      distance2(pts[0], pts[2]) < minEdge2) {
    printf("[CALIB] rejected: a raw axis spans less than 900 counts\n");
    return false;
  }

  float normal[3][3] = {};
  float rhsX[3] = {}, rhsY[3] = {};
  for (int i = 0; i < 3; ++i) {
    float v[3] = { (float)pts[i].rx, (float)pts[i].ry, 1.0f };
    for (int row = 0; row < 3; ++row) {
      rhsX[row] += v[row] * pts[i].sx;
      rhsY[row] += v[row] * pts[i].sy;
      for (int col = 0; col < 3; ++col) normal[row][col] += v[row] * v[col];
    }
  }
  float mx[3][4], my[3][4];
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) mx[row][col] = my[row][col] = normal[row][col];
    mx[row][3] = rhsX[row]; my[row][3] = rhsY[row];
  }
  if (!touchSolve3(mx, candidate.x) || !touchSolve3(my, candidate.y)) {
    printf("[CALIB] rejected: singular point geometry\n");
    return false;
  }
  float maxError = 0;
  for (int i = 0; i < 3; ++i) {
    float px = candidate.x[0] * pts[i].rx + candidate.x[1] * pts[i].ry + candidate.x[2];
    float py = candidate.y[0] * pts[i].rx + candidate.y[1] * pts[i].ry + candidate.y[2];
    float e = hypotf(px - pts[i].sx, py - pts[i].sy);
    if (e > maxError) maxError = e;
  }
  if (maxError > 18.0f) {
    printf("[CALIB] rejected: fit error %.1f px\n", maxError);
    return false;
  }
  candidate.magic = TOUCH_CAL_MAGIC;
  tcal = candidate;
  touchCalSave();
  printf("[CALIB] affine saved, fit %.1f px\n", maxError);
  printf("[CALIB] X={%.8f,%.8f,%.3f} Y={%.8f,%.8f,%.3f}\n",
         tcal.x[0], tcal.x[1], tcal.x[2], tcal.y[0], tcal.y[1], tcal.y[2]);
  lcd.fillScreen(0x0000);
  lcd.setTextColor(0x07E0, 0x0000); lcd.setTextSize(3);
  lcd.setCursor(20, H / 2 - 20); lcd.print("CALIBRATED");
  delay(1200);
  return true;
}

bool panelBegin() {
  /* ── Detection, retired as a gate ───────────────────────────────────────
     This function has been wrong about the panel twice, in opposite
     directions, and the history is worth keeping because the second mistake
     was caused by fixing the first.

     It began by asserting detection was impossible, because readCommand
     returns 0 -- true of the base class, false of the one actually in use.
     So it was replaced with a real probe, which then GATED the panel on an
     ID register that this module never answers. That turned an advisory
     check into a hard refusal of a screen that was physically present and
     working, and cost an evening.

     The panel is not sold as what it is: the silkscreen says ILI9341 and the
     controller is an ST7789. A probe that returns a false negative on the
     actual hardware is worse than no probe, so it no longer decides
     anything. It runs, it says what it saw, and the panel comes up either
     way.

        /panel?k=wuw01&mode=on    run the panel (default for this build)
        /panel?k=wuw01&mode=off   headless, for a unit with no screen
        /panel?k=wuw01&rot=1|3    which way up; both are unmirrored, 180 apart

     mode=auto is still accepted and now means the same as on, so anything
     already scripted keeps working. */
#ifndef PANEL_DEFAULT_MODE
#define PANEL_DEFAULT_MODE 1
#endif
  uint8_t mode = PANEL_DEFAULT_MODE;      // 1 run, 2 headless
  uint8_t rot  = 1;                       // 1 and 3 are the unmirrored pair
  uint32_t hz  = LCD_SPI_HZ;
  {
    Preferences pp; pp.begin("wuw", true);
    uint8_t stored = pp.getUChar("panelm", 0xFF);
    if (stored == 2) mode = 2;            // only "off" is honoured as a veto
    rot = pp.getUChar("panelrot", 1);
    if (rot != 1 && rot != 3) rot = 1;    // anything else mirrors; refuse it
    uint8_t mhz = pp.getUChar("panelhz", 0);
    if (mhz >= 4 && mhz <= 10) hz = (uint32_t)mhz * 1000000UL;
    else if (mhz > 10)
      Serial.printf("[PANEL] saved %u MHz exceeds this harness; using 10 MHz\n", mhz);
    pp.end();
  }
  if (mode == 2) {
    Serial.println("[PANEL] headless by request - /panel?k=wuw01&mode=on to run");
    printf("[PANEL] headless by request\n");
    return false;
  }

  pinMode(LCD_BL, OUTPUT);
  /* Keep the lamp dark until a hardware reset and complete ST7789 init have
     finished. This avoids presenting a stale white/noise frame at boot. */
  digitalWrite(LCD_BL, LOW);
  pinMode(LCD_CS, OUTPUT); pinMode(TCH_CS, OUTPUT); pinMode(LCD_RST, OUTPUT);
  digitalWrite(LCD_CS, HIGH); digitalWrite(TCH_CS, HIGH);
  digitalWrite(LCD_RST, LOW);
  delay(150);
  digitalWrite(LCD_RST, HIGH);
  delay(300);
  /* Display and touch share SPI2 and are serialized by the panel task. DMA
     remains off on the jumper harness so no pixel transfer can overlap a
     touch transaction or outlive its source buffer. */
  if (!lcd.setBusMode((int)hz, false)) {
    Serial.println("[PANEL] init failed - running headless");
    digitalWrite(LCD_BL, LOW);
    return false;
  }
  /* Establish a known framebuffer before exposing the backlight. */
  lcd.fillScreen(0x0000);
  /* xptRead() uses Arduino's global FSPI object. LovyanGFX owns its own bus
     wrapper and does not initialize that object for us. Use the exact pin
     setup proven by touch_diag; all accesses remain serialized on this task. */
  SPI.begin(LCD_SCLK, LCD_MISO, LCD_MOSI, -1);
  lcdOK = true;
  /* Touch borrows FSPI between synchronous display writes. */
  /* JPEGDEC writes normal host-order uint16_t RGB565. LovyanGFX's uint16_t
     image path expects setSwapBytes(true) for that representation before it
     serializes the two bytes to an SPI panel. Primitive colors are unaffected. */
  lcd.setSwapBytes(true);

  /* Never issue RDID4 here. This specific ST7789 module has no working LCD
     read path, and its SDO is physically tied to the HR2046's T_DO. Bench
     testing showed that attempting the unreadable ID leaves that shared line
     low until power is removed, making every touch channel read as zero. The
     controller type was established by the visual sweep and is fixed above. */
  Serial.println("[PANEL] ST7789 fixed profile; LCD reads disabled");
  printf("[PANEL] ST7789 fixed profile; shared SDO kept idle\n");
  lcd.setRotation(rot);
  printf("[PANEL] ST7789 %ldx%ld, rotation %u, SPI %u MHz; touch polls Z1\n",
         (long)lcd.width(), (long)lcd.height(), (unsigned)rot,
         (unsigned)(hz / 1000000));

  /* ── Geometry and integrity, stated rather than assumed ────────────────
     A screen showing a band of static next to a band of flat colour has told
     you something precise: the part that is flat IS being written and the
     part that is snow is untouched display RAM. What it has not told you is
     WHY, and the two candidates -- wrong geometry, or a bus too fast for the
     wiring -- look identical from across the bench.

     So the driver says what it thinks the panel is, and then draws patterns
     whose failure modes are distinguishable: solid fills prove coverage, a
     border proves the addressing window reaches every edge, and a fine comb
     is the first thing to break when the clock is too fast for the wires. */
  /* ── Bench diagnostics ──────────────────────────────────────────────────
   * Compiled out of the shipping firmware entirely. These exist because this
   * module is not what it is sold as -- the silkscreen says ILI9341 and the
   * controller is an ST7789 -- and its ID register never answers, so the only
   * way to identify anything about it was to paint something and look.
   *
   * env:wuwcam-forcepanel builds them in. Keep them: the next module will lie
   * about itself too.
   */
#ifdef PANEL_TOUCH_ONLY
  /* Production-integrated raw test: camera, SD, AP and servers are already
     running, but calibration and all OS hit-testing are bypassed. This makes
     the electrical reading visible without allowing a stale map to hide it. */
  lcd.fillScreen(0x0000);
  lcd.setTextColor(0x07FF, 0x0000); lcd.setTextSize(2);
  lcd.setCursor(6, 4); lcd.print("PRODUCTION TOUCH RAW");
  lcd.setTextColor(0xFFFF, 0x0000);
  lcd.setCursor(6, 28); lcd.print("press + drag anywhere");
  uint32_t seq = 0;
  for (;;) {
    int x = 0, y = 0, z = 0;
    bool down = touchSample(&x, &y, &z);
    lcd.fillRect(0, 58, PW, 86, 0x0000);
    lcd.setTextSize(3);
    lcd.setTextColor(down ? 0x07E0 : 0x7BEF, 0x0000);
    lcd.setCursor(8, 60); lcd.printf("X %4d", x);
    lcd.setCursor(8, 88); lcd.printf("Y %4d", y);
    lcd.setCursor(8, 116); lcd.printf("Z %4d %s", z, down ? "PRESS" : "IDLE ");
    if (down) {
      int px = constrain(map(x, 300, 3900, 0, PW - 1), 0, PW - 1);
      int py = constrain(map(y, 3600, 450, 0, PH - 1), 0, PH - 1);
      lcd.fillCircle(px, py, 4, 0xFFE0);
    }
    if ((++seq % 4) == 0)
      printf("[TOUCH-RAW] x=%4d y=%4d z=%4d %s\n", x, y, z,
             down ? "PRESS" : "IDLE");
    delay(50);
  }
#endif
#ifdef PANEL_SELFTEST
  for (;;) {
    /* ── Which path is corrupting bulk pixels ────────────────────────────
       The OS draws text and flat rectangles cleanly and shreds gradients and
       camera frames. Those are different code paths: solid fills write a
       repeated colour, bulk pixels go out through DMA, and on the S3 a DMA
       source in PSRAM is exactly the kind of thing that works for small
       transfers and tears on large ones.
 
       So this draws the same gradient-heavy frame four ways. If the DMA-off
       frames are clean, the bus was never the problem. */
    static const struct { int hz; bool dma; const char* label; } modes[] = {
      { 20000000, true,  "20MHz DMA ON"  },
      { 20000000, false, "20MHz DMA OFF" },
      { 10000000, true,  "10MHz DMA ON"  },
      { 10000000, false, "10MHz DMA OFF" },
    };
    for (size_t m = 0; m < sizeof(modes)/sizeof(modes[0]); m++) {
      lcd.usePanel(0, false, false, modes[m].hz);
      lcd.setBusMode(modes[m].hz, modes[m].dma);
      lcd.setRotation(rot);
      int W = lcd.width(), H = lcd.height();
      printf("[BUS] %s\n", modes[m].label);
      lcd.fillScreen(0x0000);
      /* A smooth vertical ramp is the thing that broke. Drawn as bulk pixel
         rows, the same way the skin draws its buttons. */
      static uint16_t ramp[320];
      for (int y = 0; y < 120; y++) {
        uint8_t v = (uint8_t)(y * 255 / 119);
        uint16_t c16 = lcd.color565(v, (uint8_t)(255 - v), 128);
        for (int x = 0; x < W; x++) ramp[x] = c16;
        lcd.pushImage(0, 40 + y, W, 1, ramp);
      }
      /* Horizontal ramp too: a per-pixel gradient, not a flat row. */
      for (int y = 0; y < 40; y++) {
        for (int x = 0; x < W; x++)
          ramp[x] = lcd.color565((uint8_t)(x * 255 / (W - 1)), 64,
                                 (uint8_t)(255 - x * 255 / (W - 1)));
        lcd.pushImage(0, 168 + y, W, 1, ramp);
      }
      lcd.setTextColor(0xFFFF, 0x0000);
      lcd.setTextSize(3);
      lcd.setCursor(8, 8);
      lcd.print(modes[m].label);
      lcd.setTextSize(2);
      lcd.setCursor(8, H - 22);
      lcd.print("smooth ramps = good   streaks = bad");
      delay(4000);
    }
    printf("[BUS] --- report which of the four had smooth ramps ---\n");
    /* Which of the unmirrored landscape rotations is the right way up is an
       ergonomic question -- it depends which end the ribbon leaves the case --
       so it is answered by looking, not by reasoning. */
    static const uint8_t rots[] = { 1, 3, 5, 7 };
    for (size_t r = 0; r < sizeof(rots); r++) {
      lcd.usePanel(0, false, false, LCD_SPI_HZ);     // ST7789 BGR, the measured one
      lcd.setRotation(rots[r]);
      int W = lcd.width(), H = lcd.height();
      printf("[ROT] rotation %d -> %dx%d\n", rots[r], W, H);
      lcd.fillScreen(0x0000);
      lcd.drawRect(0, 0, W, H, 0xFFFF);
      lcd.fillRect(8, 8, 40, 40, 0xF800);
      lcd.fillRect(54, 8, 40, 40, 0x07E0);
      lcd.fillRect(100, 8, 40, 40, 0x001F);
      lcd.setTextColor(0xFFFF, 0x0000);
      lcd.setTextSize(4);
      lcd.setCursor(150, 16);  lcd.print("WUW");     // reads wrong if mirrored
      lcd.setTextSize(10);
      lcd.setCursor(W / 2 - 30, H / 2 - 30);
      lcd.print((int)rots[r]);
      /* One marker per corner. A memory offset -- which ST7789 panels often
         need and ILI9341 ones do not -- shows up here as a clipped corner
         rather than as an unplaceable sense that the layout is off. */
      lcd.fillRect(0, 0, 10, 10, 0xFFE0);
      lcd.fillRect(W - 10, 0, 10, 10, 0x07FF);
      lcd.fillRect(0, H - 10, 10, 10, 0xF81F);
      lcd.fillRect(W - 10, H - 10, 10, 10, 0xFFFF);
      lcd.setTextSize(2);
      lcd.setCursor(10, H - 26);
      lcd.printf("rot %d  %dx%d", rots[r], W, H);
      delay(3500);
    }
    printf("[ROT] --- 1 and 3 are unmirrored; 5 and 7 are the same views flipped ---\n");
    lcd.usePanel(0, false, false, LCD_SPI_HZ);
    lcd.setRotation(rot);
  }
#endif

  {
    Preferences pp; pp.begin("wuw", false);
    uiSkin = pp.getUChar("uiskin", 0);
    /* v1 changes the physical-screen default to portrait-source footage
       rotated clockwise into the landscape viewfinder. Apply it once to
       existing boards, then preserve every CAMERA-menu choice afterward. */
    if (pp.getUChar("viewrotv", 0) < 1) {
      osViewRot = 1;
      pp.putUChar("viewrot", osViewRot);
      pp.putUChar("viewrotv", 1);
    } else {
      osViewRot = pp.getUChar("viewrot", 1) & 3;
    }
    pp.end();
    if (uiSkin < 0 || uiSkin >= NSKIN) uiSkin = 0;
  }
  lcd.setTextSize(1);
  lcd.setFont(&fonts::Font0);
  lcd.fillScreen(lcd.color565(SK.bg[0], SK.bg[1], SK.bg[2]));
  digitalWrite(LCD_BL, HIGH);

  viewBuf = (uint16_t*)heap_caps_malloc((size_t)PW * PH * 2, MALLOC_CAP_SPIRAM);
  /* Shared by Ghost and optional exact viewfinder strip comparison. */
  prevBuf = (uint16_t*)heap_caps_malloc((size_t)PW * PH * 2, MALLOC_CAP_SPIRAM);
  if (!prevBuf)
    Serial.println("[PANEL] previous-frame buffer unavailable - full viewfinder updates");
  fileBuf = (uint8_t*) heap_caps_malloc(FILEBUF_CAP, MALLOC_CAP_SPIRAM);
  if (!viewBuf || !fileBuf)
    Serial.println("[PANEL] no PSRAM workspace - control UI enabled; preview/gallery disabled");
  osCyberSplash();
  lightLoad();            // a manual ISO / exposure look survives a reboot
  touchCalLoad();
  {
    Preferences pp; pp.begin("wuw", false);
    bool want = pp.getBool("tcalreq", false);
    if (want) pp.putBool("tcalreq", false);       // one-shot
    pp.end();
    /* If calibration is abandoned or times out, the OS still starts -- a
       device that cannot be calibrated must still be a working camera with
       a web UI and a hardware shutter. */
    if (want || !touchCalValid()) {
      if (!touchCalibrate())
        printf("[CALIB] not completed - touch disabled, screen still runs\n");
    }
  }
  /* Calibration uses the large built-in diagnostic font. Restore the device
     OS font whether calibration completed, timed out, or was skipped. */
  lcd.setTextSize(1);
  lcd.setFont(&fonts::Font0);
  sdOk = (SD_MMC.cardType() != CARD_NONE);
  if (sdOk) galScan();

  panelUp = true;
  osRepaint = 1;
  xTaskCreatePinnedToCore(panelTaskFn, "panel", 6144, nullptr, 2, &panelTaskH, 1);
  Serial.printf("[PANEL] %ldx%ld up, skin %s\n",
                (long)lcd.width(), (long)lcd.height(), SK.name);
  printf("[PANEL] OS task running, %ldx%ld, skin %s\n",
         (long)lcd.width(), (long)lcd.height(), SK.name);
  return true;
}

void panelStop() {
  if (panelTaskH) { vTaskDelete(panelTaskH); panelTaskH = nullptr; }
  panelUp = false;
}

#endif /* WUW_PANEL */
