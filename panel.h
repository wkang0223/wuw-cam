/* ── WUW CAM panel ────────────────────────────────────────────────────────
 * The device OS, in the same binary as the camera, web server and PASAR.
 * The display is optional at runtime: if no panel answers, every call here
 * is a no-op and the camera behaves exactly as it does headless.
 *
 * Compile out entirely by leaving WUW_PANEL undefined.
 */
#pragma once
#ifdef WUW_PANEL

#include <Arduino.h>

/* Probes for a display. Returns false if none is attached -- not an error,
   just a camera without a screen. Safe to call when no panel exists. */
bool panelBegin();
bool panelPresent();
bool panelTouchReady();
bool panelSetSkin(const char* name);

/* The phone page changed VIEW ROTATE (clockwise quarter turns, 0..3). The caller has already stored
   it in NVS; this makes the glass follow at once. */
void panelSetViewRot(uint8_t quarterTurnsClockwise);

/* Started by panelBegin() on core 1. Owns the panel end to end: touch, the
   OS chrome, and the viewfinder. Core 0 remains available to WiFi. */
void panelStop();

/* What the panel shows about the rest of the system. main_s3 fills this in;
   the panel only reads it, so the two never fight over state. */
struct PanelStatus {
  bool     cam;
  bool     sd;
  bool     net;            // joined an upstream network
  uint8_t  peers;          // nearby WUW cameras in the active link group
  int      visitors;
  uint32_t photos;         // this session
  uint32_t lastImage;
  const char* device;
  const char* session;
  uint8_t  streamRes;
  uint8_t  photoRes;
  uint8_t  maxRes;
  uint8_t  streamQuality;
  uint8_t  photoQuality;
  int8_t   brightness;
  int8_t   contrast;
  int8_t   aeLevel;
  bool     hmirror;
  bool     vflip;
};
void panelPublish(const PanelStatus& s);

/* The panel asks for these rather than touching the camera itself, so a tap
   on the glass goes through the same serialised shutter as a tap on a phone. */
typedef bool (*PanelShootFn)(const char* who);
typedef bool (*PanelRecFn)(bool start);
enum PanelCameraControl {
  PANEL_CAM_STREAM_RES,
  PANEL_CAM_PHOTO_RES,
  PANEL_CAM_STREAM_QUALITY,
  PANEL_CAM_PHOTO_QUALITY,
  PANEL_CAM_BRIGHTNESS,
  PANEL_CAM_CONTRAST,
  PANEL_CAM_AE_LEVEL,
  PANEL_CAM_HMIRROR,
  PANEL_CAM_VFLIP,
  PANEL_CAM_AEC,
  PANEL_CAM_AEC_VALUE,
  PANEL_CAM_AGC,
  PANEL_CAM_AGC_GAIN,
  /* The two the LIGHT screen drives. Values are STEP INDICES, not register
     values, so the panel need know nothing about any particular sensor: how a
     stop maps to a gain or an exposure is the camera side's business, measured
     from the sensor that is actually fitted. 0 is always AUTO. */
  PANEL_CAM_ISO,           // 0 auto, 1..PANEL_ISO_STOPS-1 = 100, 200 ... 3200
  PANEL_CAM_EXPOSURE,      // 0 auto, 1..PANEL_EXP_STOPS-1 = manual, short to long
};
#define PANEL_ISO_STOPS 7
#define PANEL_EXP_STOPS 11
typedef bool (*PanelCameraFn)(PanelCameraControl control, int value);
typedef bool (*PanelCollectiveFn)(const char* tag);
/* Names the stops of PANEL_CAM_ISO and PANEL_CAM_EXPOSURE (index 0 = "AUTO").
   The camera side owns this because only it knows what the sensor can do. */
typedef const char* (*PanelLabelFn)(PanelCameraControl control, int index);
void panelHooks(PanelShootFn shoot, PanelRecFn rec,
                PanelCameraFn camera, PanelCollectiveFn collective,
                PanelLabelFn label);

/* ── Upstream WiFi, driven from the glass ──────────────────────────────────
   The access point always stays up; this is only the join to a nearby network.
   Everything is non-blocking: a touch must never wait on a radio. */
struct PanelWifiNet {
  char   ssid[33];
  int8_t rssi;             // dBm, negative
  bool   secure;           // needs a password
};
struct PanelWifiState {
  bool    connected;       // associated and holding an address
  bool    connecting;      // a join is in progress
  bool    configured;      // a network is remembered
  int8_t  rssi;
  uint8_t reason;          // last failure: 0 none, 1 wrong password, 2 not found, 3 other
  char    ssid[33];
  char    ip[16];
};
struct PanelWifiApi {
  void (*scanStart)();                                // returns at once
  int  (*scanResults)(PanelWifiNet* out, int cap);    // -1 still scanning, else count
  void (*join)(const char* ssid, const char* pass);   // remembered, non-blocking
  void (*forget)();
  void (*state)(PanelWifiState* out);
};
void panelWifiHooks(const PanelWifiApi& api);

// Adds an SD JPEG path to BWO's four remembered wall texture slots.
bool panelSetBwoWall(const char* path);

/* Exact-change strip push: off by default; noisy camera frames often change
   every strip. The old threshold argument is ignored for compatibility.
   Stats are since the last config change. */
void   panelTileConfig(int on, int thresh);
size_t panelTileStats(char* out, size_t cap);

#else
#define panelBegin()        (false)
#define panelPresent()      (false)
#define panelTouchReady()   (false)
#define panelSetSkin(n)     (false)
#define panelSetViewRot(r)  ((void)0)
#define panelStop()         ((void)0)
#define panelPublish(s)     ((void)0)
#define panelHooks(a,b,c,d,e) ((void)0)
#define panelWifiHooks(a)   ((void)0)
#define panelSetBwoWall(p)  (false)
#define panelTileConfig(a,b) ((void)0)
#define panelTileStats(o,c)  ((size_t)0)
#endif
