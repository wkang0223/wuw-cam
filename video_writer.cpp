/*
 * video_writer.cpp — MJPEG → QuickTime/MP4 recorder + effect engine
 *
 * All effects operate in the JPEG compressed domain — no decode/encode.
 *
 * JPEG datamosh principle:
 *   A JPEG frame is: [header: SOI+APP+DQT+SOF+DHT] [SOS marker] [scan data] [EOI]
 *   Swapping the scan payload between frames causes the decoder to interpret
 *   an old frame's Huffman bitstream through the current frame's tables →
 *   authentic block-corruption artefacts identical to H.264 datamosh.
 *
 * PSRAM budget (worst-case FHD):
 *   effect ring  6 × 200 KB =  1.2 MB
 *   work buffer      200 KB =  200 KB
 *   rec index  10800 ×  8 B =   87 KB
 *   camera bufs  2 × 250 KB =  500 KB
 *   ─────────────────────────────────
 *   total              ≈ 2.0 MB  (4 MB PSRAM available)
 */

#include "video_writer.h"
#include "SD_MMC.h"
#include "FS.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

// ── Compile-time limits ───────────────────────────────────────────────────
#define REC_MAX_FRAMES  10800   // 6 min @ 30 fps, 3 min @ 60 fps
#define EFX_BUF_COUNT   6       // ring slots for delay / mosh / ghost
#define EFX_BUF_SIZE    200000  // 200 KB per ring slot  (≥ HD JPEG worst-case)
#define WORK_BUF_SIZE   220000  // 220 KB output buffer

// ftyp=24, mdat header=8 → frame data starts at byte 32
#define FRAMES_OFFSET   32

// ── Runtime config (set by rec_set_config) ───────────────────────────────
static volatile uint8_t cfgFps       = 30;   // a CEILING: the sensor sets the real rate
static volatile uint8_t cfgEffect    = REC_FX_NONE;
static volatile uint8_t cfgIntensity = 5;
static volatile uint8_t cfgDelay     = 3;   // 1-6

// ── Recording state ───────────────────────────────────────────────────────
static File           recFile;
static volatile bool  recActive     = false;
static uint32_t       recFrameCount = 0;
static uint32_t*      recOffsets    = nullptr;
static uint32_t*      recSizes      = nullptr;
static uint32_t*      recTimes      = nullptr;   // capture time of each frame, ms from the first
static uint16_t       recW = 0, recH = 0;
static uint8_t        recFps        = 30;   // captured at rec_start
static uint32_t       recStartMs    = 0;
/* Which way up the picture goes, as a number of clockwise quarter turns to apply when it is
   shown. The frames are stored exactly as the sensor made them (turning a JPEG would mean
   decoding and re-encoding every one); the clip carries the turn in its track matrix instead,
   which QuickTime, iOS, Android and ffmpeg all honour. Latched at rec_start so one clip is
   never half one orientation and half another. */
static uint8_t        cfgRotation   = 0;
static uint8_t        recRotation   = 0;

// ── Effect buffers (all in PSRAM) ─────────────────────────────────────────
static uint8_t*  efxRing[EFX_BUF_COUNT] = {};   // ring of recent frames
static uint32_t  efxLen [EFX_BUF_COUNT] = {};
static uint8_t   efxHead = 0;                    // next write slot

static uint8_t*  workBuf = nullptr;              // processing scratch

// ── Big-endian write helpers ──────────────────────────────────────────────
static void w16(File& f, uint16_t v) { f.write(v>>8); f.write(v&0xFF); }
static void w32(File& f, uint32_t v) {
  f.write((v>>24)&0xFF); f.write((v>>16)&0xFF);
  f.write((v>> 8)&0xFF); f.write( v     &0xFF);
}
static void w4cc(File& f, const char* s) { f.write((const uint8_t*)s, 4); }
static void wZ(File& f, uint32_t n)      { while (n--) f.write((uint8_t)0); }

// ── JPEG helpers ──────────────────────────────────────────────────────────
// Returns byte offset of the 0xFF 0xDA (SOS) marker, or -1.
static int32_t findSOS(const uint8_t* b, size_t len) {
  for (size_t i = 0; i < len - 1; i++)
    if (b[i] == 0xFF && b[i+1] == 0xDA) return (int32_t)i;
  return -1;
}

// ── Effect engine ─────────────────────────────────────────────────────────
// Writes processed frame into workBuf. Returns output length (0 = use raw).
// All active effects automatically receive a B&W static noise pass.
static size_t applyEffect(const uint8_t* fb, size_t flen) {
  if (!workBuf || flen > WORK_BUF_SIZE) return 0;

  uint8_t  eff   = cfgEffect;
  uint8_t  inten = cfgIntensity;
  uint8_t  dly   = cfgDelay;
  size_t   outLen = 0;

  uint32_t available = min((uint32_t)EFX_BUF_COUNT - 1, (uint32_t)recFrameCount);
  if (dly > available) dly = (uint8_t)available;
  uint8_t refSlot = (uint8_t)((efxHead + EFX_BUF_COUNT - dly) % EFX_BUF_COUNT);

  switch (eff) {

  case REC_FX_GHOST:
    if (dly > 0 && efxLen[refSlot] > 0 && efxLen[refSlot] <= WORK_BUF_SIZE) {
      memcpy(workBuf, efxRing[refSlot], efxLen[refSlot]);
      outLen = efxLen[refSlot];
    }
    break;

  case REC_FX_MOSH: {
    if (dly == 0 || efxLen[refSlot] == 0) break;
    int32_t curSOS = findSOS(fb, flen);
    int32_t refSOS = findSOS(efxRing[refSlot], efxLen[refSlot]);
    if (curSOS < 0 || refSOS < 0) break;
    size_t scanLen  = flen - (size_t)curSOS;
    size_t injectAt = (size_t)curSOS + scanLen * (10 - inten) / 10;
    size_t refTail  = efxLen[refSlot] - (size_t)refSOS;
    if (injectAt + refTail > WORK_BUF_SIZE) break;
    memcpy(workBuf,            fb,                         injectAt);
    memcpy(workBuf + injectAt, efxRing[refSlot] + refSOS, refTail);
    outLen = injectAt + refTail;
    break;
  }

  case REC_FX_FREEZE: {
    uint32_t period  = (uint32_t)(12 - min((uint8_t)10, inten));
    uint32_t holdLen = inten / 2 + 1;
    if (recFrameCount % period < holdLen
        && efxLen[refSlot] > 0 && efxLen[refSlot] <= WORK_BUF_SIZE) {
      memcpy(workBuf, efxRing[refSlot], efxLen[refSlot]);
      outLen = efxLen[refSlot];
    }
    break;
  }

  case REC_FX_CORRUPT: {
    if (flen > WORK_BUF_SIZE) break;
    memcpy(workBuf, fb, flen);
    int32_t sos = findSOS(workBuf, flen);
    if (sos >= 0) {
      size_t   ss     = (size_t)sos + 2;
      size_t   slen   = flen - ss;
      size_t   stride = max((size_t)32, slen / 64);
      uint32_t thresh = (uint32_t)inten * 25;
      for (size_t i = ss; i + stride < flen - 1; i += stride) {
        if ((uint32_t)(random(256)) < thresh) {
          size_t blk = (i - ss) / stride;
          if (blk == 0) continue;
          size_t src = ss + ((size_t)random((long)blk) * stride);
          for (size_t j = 0; j < stride && (i+j) < flen-1; j++) {
            if (workBuf[i+j] != 0xFF && workBuf[src+j] != 0xFF)
              workBuf[i+j] = workBuf[src+j];
          }
        }
      }
    }
    outLen = flen;
    break;
  }

  case REC_FX_STROBE: {
    uint32_t hold     = (uint32_t)(inten / 2 + 1);
    uint8_t  lastSlot = (uint8_t)((efxHead + EFX_BUF_COUNT - 1) % EFX_BUF_COUNT);
    if (recFrameCount % (hold + 1) != 0
        && efxLen[lastSlot] > 0 && efxLen[lastSlot] <= WORK_BUF_SIZE) {
      memcpy(workBuf, efxRing[lastSlot], efxLen[lastSlot]);
      outLen = efxLen[lastSlot];
    }
    break;
  }

  default: break;
  }

  // ── B&W noise pass (all effects) ─────────────────────────────────────
  // XOR LCG-random bytes into JPEG scan data → film grain / static look.
  // Sensor desaturation (rec_set_config) makes output truly greyscale.
  if (outLen > 0 && inten > 0) {
    int32_t sos = findSOS(workBuf, outLen);
    if (sos >= 0) {
      size_t   ss   = (size_t)sos + 2;
      uint32_t step = 512u / (uint32_t)inten;
      if (step < 8u) step = 8u;
      uint32_t seed = recFrameCount * 2654435761UL;
      for (size_t i = ss; i < outLen - 1; i += step) {
        seed = seed * 1664525u + 1013904223u;
        uint8_t nb = (seed >> 16) & 0xFF;
        if (nb && nb != 0xFF && workBuf[i] != 0xFF)
          workBuf[i] ^= nb;
      }
    }
  }

  return outLen;
}

// ── MOOV writer ────────────────────────────────────────────────────────────
// Box sizes (N = frame count):
//   stco=16+N*4  stsz=20+N*4  stsc=28  stts=24  stsd=102
//   stbl=198+N*8  minf=262+N*8  mdia=347+N*8
//   trak=447+N*8  moov=563+N*8
/* The index used to say every frame lasts exactly 1000/fps milliseconds.
 * That is only true if frames arrive on a metronome, and they never did: the
 * camera is shared, SD writes stall, and the sensor has its own cadence. A clip
 * whose frames really came at 40, 40, 120, 40 ms but is labelled 40, 40, 40, 40
 * plays back with the pauses squeezed out -- fast, then juddering.
 *
 * Each frame now carries its real capture time, and the stts (time-to-sample)
 * table records the real gap to the next one, run-length encoded so a steady
 * clip still costs only a few entries. Every enclosing atom size grows by the
 * same amount the table does. */
static uint32_t frameDelta(uint32_t i, uint32_t N, uint32_t fallbackMs) {
  if (N < 2) return fallbackMs;
  if (i + 1 < N) { uint32_t d = recTimes[i + 1] - recTimes[i]; return d ? d : 1; }
  uint32_t d = recTimes[N - 1] - recTimes[N - 2];           // last frame: same as the one before
  return d ? d : fallbackMs;
}

static void writeMoov(File& f, uint32_t N, uint16_t W, uint16_t H) {
  const uint32_t TS = 1000;
  const uint32_t fallback = TS / (recFps ? recFps : 30);

  uint32_t E = 0, DUR = 0, runDelta = 0;
  for (uint32_t i = 0; i < N; ++i) {
    uint32_t d = frameDelta(i, N, fallback);
    DUR += d;
    if (i == 0 || d != runDelta) { ++E; runDelta = d; }
  }
  const uint32_t X = 8 * (E - 1);       // growth of stts over the single-entry layout

  w32(f,563+N*8+X); w4cc(f,"moov");

  // mvhd (108)
  w32(f,108); w4cc(f,"mvhd");
  w32(f,0); w32(f,0); w32(f,0); w32(f,TS); w32(f,DUR);
  w32(f,0x00010000); w16(f,0x0100); wZ(f,10);
  w32(f,0x00010000);w32(f,0);w32(f,0);
  w32(f,0);w32(f,0x00010000);w32(f,0);
  w32(f,0);w32(f,0);w32(f,0x40000000);
  wZ(f,24); w32(f,2);

  // trak
  w32(f,447+N*8+X); w4cc(f,"trak");

  // tkhd (92)
  w32(f,92); w4cc(f,"tkhd");
  w32(f,0x00000003); w32(f,0); w32(f,0); w32(f,1); w32(f,0); w32(f,DUR);
  wZ(f,8); w16(f,0); w16(f,0); w16(f,0); w16(f,0);
  /* Track matrix (16.16 fixed point; a point maps to (a*x + c*y + tx, b*x + d*y + ty)).
     Quarter turns clockwise. The translation keeps the turned picture in positive space:
     a 90 degree turn of a W x H frame needs tx = H, a 270 needs ty = W, and so on. */
  {
    const uint32_t ONE = 0x00010000u, NEG = 0xFFFF0000u;
    uint32_t a = ONE, b = 0, c = 0, d = ONE, tx = 0, ty = 0;
    switch (recRotation & 3) {
      case 1: a = 0;   b = ONE; c = NEG; d = 0;   tx = (uint32_t)H << 16; break;   // 90 clockwise
      case 2: a = NEG; b = 0;   c = 0;   d = NEG; tx = (uint32_t)W << 16; ty = (uint32_t)H << 16; break;
      case 3: a = 0;   b = NEG; c = ONE; d = 0;   ty = (uint32_t)W << 16; break;   // 90 counter-clockwise
      default: break;
    }
    w32(f,a);  w32(f,b);  w32(f,0);
    w32(f,c);  w32(f,d);  w32(f,0);
    w32(f,tx); w32(f,ty); w32(f,0x40000000);
  }
  w32(f,(uint32_t)W<<16); w32(f,(uint32_t)H<<16);

  // mdia
  w32(f,347+N*8+X); w4cc(f,"mdia");

  // mdhd (32)
  w32(f,32); w4cc(f,"mdhd");
  w32(f,0); w32(f,0); w32(f,0); w32(f,TS); w32(f,DUR);
  w16(f,0x55C4); w16(f,0);

  // hdlr (45)
  w32(f,45); w4cc(f,"hdlr");
  w32(f,0); w32(f,0); w4cc(f,"vide"); wZ(f,12);
  f.write((const uint8_t*)"VideoHandler\0",13);

  // minf
  w32(f,262+N*8+X); w4cc(f,"minf");

  // vmhd (20)
  w32(f,20); w4cc(f,"vmhd"); w32(f,0x00000001); w32(f,0); w32(f,0);

  // dinf (36) -> dref (28) -> url (12)
  w32(f,36); w4cc(f,"dinf");
  w32(f,28); w4cc(f,"dref"); w32(f,0); w32(f,1);
  w32(f,12); w4cc(f,"url "); w32(f,0x00000001);

  // stbl
  w32(f,198+N*8+X); w4cc(f,"stbl");

  // stsd (102) -> jpeg (86)
  w32(f,102); w4cc(f,"stsd"); w32(f,0); w32(f,1);
  w32(f,86);  w4cc(f,"jpeg");
  wZ(f,6); w16(f,1); wZ(f,16);
  w16(f,W); w16(f,H);
  w32(f,0x00480000); w32(f,0x00480000); w32(f,0);
  w16(f,1);
  f.write((uint8_t)11); f.write((const uint8_t*)"Motion JPEG",11); wZ(f,20);
  w16(f,24); w16(f,0xFFFF);

  // stts: one entry per run of equal frame durations
  w32(f,16+8*E); w4cc(f,"stts"); w32(f,0); w32(f,E);
  {
    uint32_t count = 0, cur = 0;
    for (uint32_t i = 0; i < N; ++i) {
      uint32_t d = frameDelta(i, N, fallback);
      if (count && d != cur) { w32(f,count); w32(f,cur); count = 0; }
      cur = d; ++count;
    }
    if (count) { w32(f,count); w32(f,cur); }
  }

  // stsc (28)
  w32(f,28); w4cc(f,"stsc"); w32(f,0); w32(f,1);
  w32(f,1); w32(f,1); w32(f,1);

  // stsz (20+N*4)
  w32(f,20+N*4); w4cc(f,"stsz"); w32(f,0); w32(f,0); w32(f,N);
  for (uint32_t i = 0; i < N; i++) w32(f,recSizes[i]);

  // stco (16+N*4)
  w32(f,16+N*4); w4cc(f,"stco"); w32(f,0); w32(f,N);
  for (uint32_t i = 0; i < N; i++) w32(f,recOffsets[i]);
}

// ═══════════════════════════════════════════════════════════════════════════
//  RECORDING PIPELINE
//
//  camera ──► [capture task] ──► ring of PSRAM slots ──► [writer task] ──► SD
//                  │
//                  └──► preview slot ──► viewfinder, /jpg, /stream
//
//  The old recorder was one loop: wait for a frame, write it to the card,
//  sleep. Three things made that stutter.
//
//   1. THE CARD STALLED THE CAMERA. A FAT write that has to allocate a cluster
//      can take tens of milliseconds, and the loop could not take the next
//      frame until it finished -- so the frame that arrived meanwhile was
//      simply lost. A ring between the two decouples them: the capture task
//      only copies a frame into memory, and the writer works through the queue
//      at the card's pace.
//   2. THE CAMERA WAS SHARED. The panel's viewfinder, every browser polling
//      /jpg and the /stream endpoint all called esp_camera_fb_get() too, and a
//      frame goes to exactly ONE caller. The recorder got an irregular
//      fraction of the sensor's frames. While recording, this task is the only
//      reader, and everyone else is handed its latest frame (see camFrame*).
//   3. FRAMES WERE TIMED AS IF THEY CAME ON A METRONOME. Each now carries the
//      driver's capture timestamp and the index records the real gaps.
// ═══════════════════════════════════════════════════════════════════════════
#define REC_MAX_SLOTS    24
#define REC_MIN_SLOTS    3
/* PSRAM the ring may use. A deeper ring is the margin that absorbs an SD card's occasional
   slow write without dropping a frame, so it is worth as much memory as can be spared: the
   caller frees what a recording does not need first (see recModeEnter in main_s3.cpp), and
   the sizing below still leaves REC_PSRAM_HEADROOM for everything else. */
#define REC_RING_BUDGET  (4u * 1024u * 1024u)
#define REC_PSRAM_HEADROOM (768u * 1024u)
#define PREVIEW_SLOTS    2

struct RecSlot { uint8_t* buf; uint32_t len; uint32_t tsMs; };
static RecSlot         ring[REC_MAX_SLOTS];
static uint8_t         ringN = 0;
static uint32_t        slotCap = 0;
static QueueHandle_t   freeQ = nullptr, fullQ = nullptr;
static volatile bool   capRunning = false, wrRunning = false;
static TaskHandle_t    capTask = nullptr, wrTask = nullptr;

// What went wrong and how well it went: reported when the clip stops.
static volatile uint32_t stCaptured, stDropped, stOversize, stPeakQueue, stWriteMsMax;
static volatile uint32_t stWrites;
static uint64_t        stWriteMsSum;
static char            recError[16] = "";
static char            recLast[96]  = "";

// ── Preview: the latest frame, for everyone who is not the recorder ───────
struct PrevSlot { uint8_t* buf; uint32_t cap, len; uint16_t w, h; int8_t leases; };
static PrevSlot          prev[PREVIEW_SLOTS];
static volatile int8_t   prevLatest = -1;
static volatile uint32_t prevSeq = 0;      // bumps once per published frame
static SemaphoreHandle_t prevMux = nullptr;

static void previewPublish(const uint8_t* b, size_t n, uint16_t w, uint16_t h) {
  if (!prevMux || !prev[0].buf) return;
  int target = -1;
  xSemaphoreTake(prevMux, portMAX_DELAY);
  for (int k = 0; k < PREVIEW_SLOTS; ++k)
    if (k != prevLatest && prev[k].leases == 0) { target = k; break; }
  xSemaphoreGive(prevMux);
  if (target < 0 || n > prev[target].cap) return;       // readers are busy: skip, never tear
  /* A slot that is not the latest is never handed out, and leases==0 means no
     earlier reader still holds it, so copying outside the lock is safe. */
  memcpy(prev[target].buf, b, n);
  prev[target].len = (uint32_t)n; prev[target].w = w; prev[target].h = h;
  xSemaphoreTake(prevMux, portMAX_DELAY);
  prevLatest = (int8_t)target;
  ++prevSeq;
  xSemaphoreGive(prevMux);
}

bool camFrameAcquire(CamFrame* f) {
  memset(f, 0, sizeof(*f)); f->slot = -1;
  if (!recActive) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return false;
    f->buf = fb->buf; f->len = fb->len; f->w = (uint16_t)fb->width; f->h = (uint16_t)fb->height;
    f->fb = fb;
    return true;
  }
  // Recording: the recorder owns the sensor. Wait briefly for its first frame.
  for (int tries = 0; tries < 12; ++tries) {
    xSemaphoreTake(prevMux, portMAX_DELAY);
    int k = prevLatest;
    if (k >= 0) {
      ++prev[k].leases;
      f->buf = prev[k].buf; f->len = prev[k].len; f->w = prev[k].w; f->h = prev[k].h;
      f->slot = (int8_t)k;
      f->seq = prevSeq;
      xSemaphoreGive(prevMux);
      return true;
    }
    xSemaphoreGive(prevMux);
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  return false;
}

void camFrameRelease(CamFrame* f) {
  if (f->fb) esp_camera_fb_return((camera_fb_t*)f->fb);
  else if (f->slot >= 0 && prevMux) {
    xSemaphoreTake(prevMux, portMAX_DELAY);
    if (prev[f->slot].leases > 0) --prev[f->slot].leases;
    xSemaphoreGive(prevMux);
  }
  f->buf = nullptr; f->fb = nullptr; f->slot = -1;
}

// ── Capture: take each frame off the sensor and get out of the way ────────
static uint64_t frameTimeUs(const camera_fb_t* fb) {
  uint64_t t = (uint64_t)fb->timestamp.tv_sec * 1000000ull + (uint64_t)fb->timestamp.tv_usec;
  return t ? t : (uint64_t)esp_timer_get_time();       // driver did not stamp it: use the clock
}

static void recCaptureFn(void*) {
  uint64_t firstUs = 0;
  int64_t  dueUs = 0;
  while (recActive) {
    camera_fb_t* fb = esp_camera_fb_get();             // blocks for the next frame
    if (!fb) { vTaskDelay(5 / portTICK_PERIOD_MS); continue; }
    const uint64_t tUs = frameTimeUs(fb);
    if (!firstUs) {
      firstUs = tUs; dueUs = (int64_t)tUs;
      recW = (uint16_t)fb->width; recH = (uint16_t)fb->height;
    }
    previewPublish(fb->buf, fb->len, (uint16_t)fb->width, (uint16_t)fb->height);
    ++stCaptured;

    /* cfgFps is a ceiling, not a target. Frames are taken when they fall on or
       near the next slot of that grid, so a sensor running at 25 fps under a
       30 ceiling is recorded in full, and a 60 fps sensor under a 30 ceiling
       is thinned evenly -- never slept into an irregular rhythm. */
    const int64_t interval = 1000000 / (cfgFps ? cfgFps : 30);
    if ((int64_t)tUs >= dueUs - interval / 8) {
      dueUs += interval;
      if ((int64_t)tUs - dueUs > 2 * interval) dueUs = (int64_t)tUs + interval;   // after a gap, resync
      uint8_t id;
      if (fb->len > slotCap) {
        ++stOversize;                                  // would not fit a slot; never truncate a JPEG
      } else if (xQueueReceive(freeQ, &id, 0) == pdTRUE) {
        memcpy(ring[id].buf, fb->buf, fb->len);
        ring[id].len = (uint32_t)fb->len;
        ring[id].tsMs = (uint32_t)((tUs - firstUs) / 1000ull);
        xQueueSend(fullQ, &id, 0);
        uint32_t q = (uint32_t)uxQueueMessagesWaiting(fullQ);
        if (q > stPeakQueue) stPeakQueue = q;
      } else {
        ++stDropped;                                   // the card fell more than a ring behind
      }
    }
    esp_camera_fb_return(fb);
  }
  capRunning = false;
  capTask = nullptr;
  vTaskDelete(nullptr);
}

// ── Writer: drain the ring to the card at the card's pace ─────────────────
static void recWriterFn(void*) {
  for (;;) {
    uint8_t id;
    if (xQueueReceive(fullQ, &id, 50 / portTICK_PERIOD_MS) != pdTRUE) {
      if (!capRunning) break;                          // capture is done and the ring is empty
      continue;
    }
    RecSlot& sl = ring[id];
    if (recFrameCount < REC_MAX_FRAMES && recError[0] == 0) {
      const uint8_t* outBuf = sl.buf;
      size_t outLen = sl.len;
      if (cfgEffect != REC_FX_NONE) {
        size_t efxOut = applyEffect(sl.buf, sl.len);
        if (efxOut > 0) { outBuf = workBuf; outLen = efxOut; }
      }
      // History for the effects must hold the RAW frame, pushed after they read it.
      if (efxRing[efxHead] && sl.len <= EFX_BUF_SIZE) {
        memcpy(efxRing[efxHead], sl.buf, sl.len);
        efxLen[efxHead] = sl.len;
        efxHead = (efxHead + 1) % EFX_BUF_COUNT;
      }
      uint32_t t0 = millis();
      uint32_t at = (uint32_t)recFile.position();
      size_t wrote = recFile.write(outBuf, outLen);
      uint32_t ms = millis() - t0;
      if (wrote != outLen) {
        /* The old loop never checked. A full or failing card silently produced
           a clip with the tail missing and no hint why. Stop, say so, and keep
           what is already safely on the card. */
        snprintf(recError, sizeof(recError), "SD_FULL");
        recActive = false;
      } else {
        recOffsets[recFrameCount] = at;
        recSizes  [recFrameCount] = (uint32_t)outLen;
        recTimes  [recFrameCount] = sl.tsMs;
        ++recFrameCount;
        stWriteMsSum += ms; ++stWrites;
        if (ms > stWriteMsMax) stWriteMsMax = ms;
      }
    } else if (recFrameCount >= REC_MAX_FRAMES) {
      recActive = false;                               // auto-stop at the cap
    }
    xQueueSend(freeQ, &id, 0);
  }
  wrRunning = false;
  wrTask = nullptr;
  vTaskDelete(nullptr);
}

// ── Public API ────────────────────────────────────────────────────────────
void rec_set_rotation(uint8_t quarterTurnsClockwise) { cfgRotation = quarterTurnsClockwise & 3; }

void rec_set_config(uint8_t fps, uint8_t effect,
                    uint8_t intensity, uint8_t delayFrames) {
  cfgFps       = max((uint8_t)1,  min((uint8_t)60, fps));
  cfgEffect    = effect;
  cfgIntensity = min((uint8_t)10, intensity);
  cfgDelay     = max((uint8_t)1,  min((uint8_t)(EFX_BUF_COUNT-1), delayFrames));
  /* Desaturate the sensor while a glitch effect is active (black and white output), and give back
     exactly what was there before when it stops: a person's own saturation setting is not ours to
     overwrite with 0 every time the recorder is configured. */
  static bool    desaturated = false;
  static int8_t  satBefore   = 0;
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    if (effect && !desaturated) { satBefore = s->status.saturation; s->set_saturation(s, -2); desaturated = true; }
    else if (!effect && desaturated) { s->set_saturation(s, satBefore); desaturated = false; }
  }
}

static void ringFree() {
  for (int i = 0; i < REC_MAX_SLOTS; ++i) {
    if (ring[i].buf) { heap_caps_free(ring[i].buf); ring[i].buf = nullptr; }
  }
  ringN = 0;
  if (freeQ) { vQueueDelete(freeQ); freeQ = nullptr; }
  if (fullQ) { vQueueDelete(fullQ); fullQ = nullptr; }
}

/* Slot size follows the frame size: a JPEG at the quality used here is well
   under a quarter of its pixel count in bytes. Fixed 200 KB slots would waste
   most of the ring on small frames, or overflow on big ones. */
static uint32_t slotBytesFor(uint16_t w, uint16_t h) {
  uint32_t b = ((uint32_t)w * h) / 4u;
  if (b < 48u * 1024u)  b = 48u * 1024u;
  if (b > 400u * 1024u) b = 400u * 1024u;
  return b;
}

bool rec_start(const char* path, bool hasPsram) {
  if (recActive || capTask || wrTask) return false;
  if (!hasPsram) return false;                         // the ring lives in PSRAM

  // ── index tables ──
  if (!recOffsets) {
    uint32_t bytes = REC_MAX_FRAMES * sizeof(uint32_t);
    recOffsets = (uint32_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    recSizes   = (uint32_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    recTimes   = (uint32_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!recOffsets || !recSizes || !recTimes) return false;
  }
  if (!recTimes) {                                     // tables from an older allocation
    recTimes = (uint32_t*)heap_caps_malloc(REC_MAX_FRAMES * sizeof(uint32_t), MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!recTimes) return false;
  }

  // ── effect buffers: only when an effect is actually selected ──
  if (cfgEffect != REC_FX_NONE && !workBuf) {
    workBuf = (uint8_t*)heap_caps_malloc(WORK_BUF_SIZE, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    for (int i = 0; i < EFX_BUF_COUNT; i++)
      if (!efxRing[i])
        efxRing[i] = (uint8_t*)heap_caps_malloc(EFX_BUF_SIZE, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  }
  for (int i = 0; i < EFX_BUF_COUNT; i++) efxLen[i] = 0;
  efxHead = 0;

  // ── size the ring from the current frame size and what PSRAM can spare ──
  uint16_t w = 800, h = 600;
  {
    camera_fb_t* probe = esp_camera_fb_get();          // one frame, only to learn the size
    if (probe) { w = (uint16_t)probe->width; h = (uint16_t)probe->height; esp_camera_fb_return(probe); }
  }
  slotCap = slotBytesFor(w, h);
  uint32_t free_ = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  uint32_t budget = min(REC_RING_BUDGET, free_ > REC_PSRAM_HEADROOM ? free_ - REC_PSRAM_HEADROOM : 0u);
  uint32_t want = budget / slotCap;
  if (want > REC_MAX_SLOTS) want = REC_MAX_SLOTS;
  if (want < REC_MIN_SLOTS) {
    Serial.printf("[REC] not enough PSRAM for a ring (%u free, slots of %u) - refusing\n",
                  (unsigned)free_, (unsigned)slotCap);
    return false;
  }
  freeQ = xQueueCreate(REC_MAX_SLOTS, sizeof(uint8_t));
  fullQ = xQueueCreate(REC_MAX_SLOTS, sizeof(uint8_t));
  if (!freeQ || !fullQ) { ringFree(); return false; }
  for (uint32_t i = 0; i < want; ++i) {
    ring[i].buf = (uint8_t*)heap_caps_malloc(slotCap, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!ring[i].buf) break;
    ++ringN;
    uint8_t id = (uint8_t)i; xQueueSend(freeQ, &id, 0);
  }
  if (ringN < REC_MIN_SLOTS) { ringFree(); return false; }

  // ── preview slots, shared with everyone who is not the recorder ──
  if (!prevMux) prevMux = xSemaphoreCreateMutex();
  for (int k = 0; k < PREVIEW_SLOTS; ++k) {
    if (!prev[k].buf) {
      prev[k].cap = slotCap;
      prev[k].buf = (uint8_t*)heap_caps_malloc(slotCap, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    } else if (prev[k].cap < slotCap) {                // frame size grew since last clip
      heap_caps_free(prev[k].buf);
      prev[k].cap = slotCap;
      prev[k].buf = (uint8_t*)heap_caps_malloc(slotCap, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    }
    if (!prev[k].buf) { ringFree(); return false; }
    prev[k].leases = 0; prev[k].len = 0;
  }
  prevLatest = -1;

  // ── file ──
  recFile = SD_MMC.open(path, FILE_WRITE);
  if (!recFile) { ringFree(); return false; }

  recFrameCount = 0;
  recW = recH   = 0;
  recRotation   = cfgRotation;
  recFps        = cfgFps;
  recStartMs    = millis();
  stCaptured = stDropped = stOversize = stPeakQueue = stWriteMsMax = stWrites = 0;
  stWriteMsSum = 0; recError[0] = 0;

  // ftyp (24 B) — QuickTime + mp41 compatible brands
  const uint8_t ftyp[24] = {
    0,0,0,24,'f','t','y','p',
    'q','t',' ',' ', 0,0,2,0,
    'q','t',' ',' ','m','p','4','1'
  };
  recFile.write(ftyp, 24);
  const uint8_t mdat[8] = {0,0,0,0,'m','d','a','t'};   // size patched in rec_stop
  recFile.write(mdat, 8);                              // frame data begins at byte 32

  recActive = true; capRunning = true; wrRunning = true;
  /* The writer sits on core 0 -- an SD write blocks on the card, and core 1 is
     where the camera, the web server and the panel already compete. Capture
     stays on core 1 beside the camera interrupt, one priority above the panel
     so a frame is never missed because the screen was busy. */
  xTaskCreatePinnedToCore(recWriterFn,  "recw", 6144, nullptr, 3, &wrTask,  0);
  xTaskCreatePinnedToCore(recCaptureFn, "recc", 4096, nullptr, 4, &capTask, 1);
  Serial.printf("[REC] ring %u x %u KB for %ux%u, ceiling %u fps\n",
                (unsigned)ringN, (unsigned)(slotCap / 1024), (unsigned)w, (unsigned)h, (unsigned)cfgFps);
  return true;
}

/* Everything the recorder holds that only a recording needs, released the moment a clip ends: the
   effect ring and work buffer (about 1.4 MB once an effect has been used) and the shared preview
   slots (up to 800 KB). Left parked, they sat in PSRAM between clips and shrank what the next
   recording's ring, the pre-roll and the gallery could get. A preview slot a web handler still
   holds is left alone; the next rec_start reuses or resizes it. */
static void efxFree() {
  if (workBuf) { heap_caps_free(workBuf); workBuf = nullptr; }
  for (int i = 0; i < EFX_BUF_COUNT; i++) {
    if (efxRing[i]) { heap_caps_free(efxRing[i]); efxRing[i] = nullptr; }
    efxLen[i] = 0;
  }
}
static void previewFree() {
  if (!prevMux) return;
  xSemaphoreTake(prevMux, portMAX_DELAY);
  for (int k = 0; k < PREVIEW_SLOTS; ++k) {
    if (prev[k].buf && prev[k].leases == 0) {
      heap_caps_free(prev[k].buf);
      prev[k].buf = nullptr; prev[k].cap = 0; prev[k].len = 0;
    }
  }
  prevLatest = -1;
  xSemaphoreGive(prevMux);
}

String rec_stop() {
  if (!recActive && !capTask && !wrTask && recFrameCount == 0) return "";
  recActive = false;

  for (int i = 0; i < 40 && capTask != nullptr; i++) vTaskDelay(50 / portTICK_PERIOD_MS);
  if (capTask != nullptr) {
    recFile.close();
    Serial.println("[REC] capture task did not exit - file left without moov");
    return "ERR_TASK_STUCK";
  }
  // The writer finishes the ring: up to a ring's worth of frames still to write.
  for (int i = 0; i < 160 && wrTask != nullptr; i++) vTaskDelay(50 / portTICK_PERIOD_MS);
  if (wrTask != nullptr) {
    recFile.close();
    Serial.println("[REC] writer task did not drain - file left without moov");
    return "ERR_TASK_STUCK";
  }
  ringFree();
  efxFree();
  previewFree();
  if (recFrameCount == 0) { recFile.close(); return recError[0] ? String("ERR_") + recError : String("ERR_NO_FRAMES"); }

  uint32_t endOfFrames = (uint32_t)recFile.position();
  uint32_t mdatSize    = endOfFrames - 24;
  uint8_t  sz[4] = { (uint8_t)(mdatSize>>24),(uint8_t)(mdatSize>>16),
                     (uint8_t)(mdatSize>> 8),(uint8_t) mdatSize };
  recFile.seek(24);
  recFile.write(sz, 4);
  recFile.seek(endOfFrames);

  writeMoov(recFile, recFrameCount, recW, recH);

  uint32_t totalBytes = (uint32_t)recFile.position();
  uint32_t spanMs = recFrameCount > 1 ? recTimes[recFrameCount - 1] - recTimes[0] : 0;
  float    fps    = spanMs ? (recFrameCount - 1) * 1000.0f / spanMs : 0.0f;
  recFile.close();

  uint32_t avgWrite = stWrites ? (uint32_t)(stWriteMsSum / stWrites) : 0;
  snprintf(recLast, sizeof(recLast),
           "%lu frames, %.1f fps, %lu dropped, ring peak %lu/%u, write avg %lu max %lu ms%s%s",
           (unsigned long)recFrameCount, fps, (unsigned long)stDropped,
           (unsigned long)stPeakQueue, (unsigned)ringN, (unsigned long)avgWrite,
           (unsigned long)stWriteMsMax, recError[0] ? " ERROR " : "", recError);
  Serial.printf("[REC] %s | %lus | %luKB | %ux%u | fx=%u\n", recLast,
                (unsigned long)((millis() - recStartMs) / 1000), (unsigned long)(totalBytes / 1024),
                (unsigned)recW, (unsigned)recH, (unsigned)cfgEffect);

  String out = String(recFrameCount) + "fr " + String((millis() - recStartMs) / 1000) + "s " +
               String(fps, 1) + "fps";
  if (stDropped) out += " " + String((unsigned long)stDropped) + "drop";
  if (recError[0]) out += String(" ") + recError;
  return out;
}

bool     rec_is_active()    { return recActive; }
uint32_t rec_frame_count()  { return recFrameCount; }
uint32_t rec_elapsed_secs() { return recActive ? (millis()-recStartMs)/1000 : 0; }

size_t rec_stats_json(char* out, size_t cap) {
  uint32_t span = recFrameCount > 1 ? recTimes[recFrameCount - 1] - recTimes[0] : 0;
  float fps = (recActive && span) ? (recFrameCount - 1) * 1000.0f / span : 0.0f;
  return (size_t)snprintf(out, cap,
    "{\"active\":%s,\"frames\":%lu,\"fps\":%.1f,\"ceiling\":%u,\"captured\":%lu,\"dropped\":%lu,"
    "\"peak\":%lu,\"slots\":%u,\"writeMax\":%lu,\"error\":\"%s\",\"last\":\"%s\"}",
    recActive ? "true" : "false", (unsigned long)recFrameCount, fps, (unsigned)cfgFps,
    (unsigned long)stCaptured, (unsigned long)stDropped, (unsigned long)stPeakQueue,
    (unsigned)ringN, (unsigned long)stWriteMsMax, recError, recLast);
}
