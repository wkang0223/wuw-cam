#pragma once
#include "Arduino.h"

// ── Effect types ──────────────────────────────────────────────────────────
// All effects operate on compressed JPEG data (no decode/encode).
typedef enum {
  REC_FX_NONE    = 0,  // Clean output
  REC_FX_GHOST   = 1,  // Output frame from N frames ago
  REC_FX_MOSH    = 2,  // Datamosh: swap JPEG scan payload between frames
  REC_FX_FREEZE  = 3,  // Stutter: repeat frozen frame periodically
  REC_FX_CORRUPT = 4,  // Block corruption: swap chunks in scan data
  REC_FX_STROBE  = 5,  // Repeat last frame N times (strobe / time-warp)
} RecEffect;

// ── Config (call any time; takes effect on next frame) ────────────────────
// fps         : a CEILING, 1..60. The sensor sets the real rate (about 25 fps
//               at SVGA); frames are recorded in full up to this limit and
//               thinned evenly above it.
// effect      : RecEffect value
// intensity   : 0-10  (effect strength)
// delayFrames : 1-6   (ghost/mosh reference lag in frames)
void rec_set_config(uint8_t fps, uint8_t effect,
                    uint8_t intensity, uint8_t delayFrames);

// How the picture should be turned when it is shown, in clockwise quarter turns
// (0..3). The recorder never re-encodes: frames stay exactly as the sensor made
// them and the turn is written into the clip's track matrix. Takes effect for
// the NEXT clip.
void rec_set_rotation(uint8_t quarterTurnsClockwise);

// ── Recording API ─────────────────────────────────────────────────────────
bool     rec_start(const char* path, bool hasPsram);
String   rec_stop();
bool     rec_is_active();
uint32_t rec_frame_count();
uint32_t rec_elapsed_secs();

// ── Sharing the camera ────────────────────────────────────────────────────
// A frame off the sensor goes to exactly ONE caller of esp_camera_fb_get().
// While recording, the recorder must be that caller, so everything else that
// wants a picture asks here: normally this is a real frame straight off the
// sensor; during a recording it is the recorder's latest frame, shared and
// read-only. Always pair acquire with release. buf is valid until release.
struct CamFrame {
  const uint8_t* buf;
  size_t         len;
  uint16_t       w, h;
  void*          fb;     // the camera's own frame when not recording
  int8_t         slot;   // the shared preview slot when recording
  uint32_t       seq;    // shared frames only: changes whenever a NEW frame was published
};
bool camFrameAcquire(CamFrame* f);
void camFrameRelease(CamFrame* f);

// JSON snapshot of the recorder: frames, real fps, dropped frames, ring depth.
size_t rec_stats_json(char* out, size_t cap);
