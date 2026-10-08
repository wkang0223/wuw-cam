# WUW CAM OS in Rust: alternative firmware

Status: design approved for Phase 1 (spikes). Phase 2 gets its own spec.

## Goal

Build a second, independent firmware for the WUW CAM in Rust, with the same
hardware and the same user-visible behaviour, aiming at lower latency, more
deterministic real-time control and better camera/display throughput. It lives
beside the existing C++ firmware. Neither replaces the other; a user picks one
at flash time.

## Non-goals

- No hardware changes. Pin map is `docs/WIRING.md` (verified by
  `tools/pincheck.py`).
- No removal or refactor of the C++ firmware as part of this work.
- Rust is not expected to beat hardware limits. Measured limits stay in force:
  OV5640 at 24 MHz XCLK runs 5-6 fps in SXGA and above and ~28 fps at 720p;
  a 320x240 SPI push is 19 ms at 80 MHz. Rust can only win on software
  overhead, scheduling and memory behaviour.

## Coexistence rules

1. **Location:** `rust/` is a Cargo workspace. The C++ tree and PlatformIO
   environments are untouched.
2. **Settings compatibility:** the Rust firmware uses the same partition table
   and the same NVS namespaces and keys as the C++ firmware (WiFi `ssid`/`pass`,
   AP password `appass`, `streamres`, BWO name), so switching firmware keeps a
   camera's settings.
3. **Shared web UI:** the browser UI (HTML/JS and the 60 WebGL effects) is
   embedded in `main_s3.cpp` today. Phase 2 extracts it to shared asset files
   that both firmwares serve, so the UI has one source.
4. **Safe flashing:** before the first Rust flash on any camera, read back its
   full flash (16 MB) as a backup. Spikes flash the app image only and must not
   erase the NVS partition.

## Phase 1: feasibility spikes

Three independent crates under `rust/spikes/`, each printing a PASS/FAIL report
with numbers over serial.

| Spike | Pass condition | Fallback if it fails |
|---|---|---|
| A: camera | OV5640 init over SCCB; 720p JPEG frames over the S3 LCD_CAM interface; fps measured against the C++ baseline (~28 fps at 720p, 24 MHz XCLK) | C `esp32-camera` driver via Rust bindings |
| B: SD card | Mount on the existing 1-bit SDMMC wiring (GPIO 38/39/40); write and read back a 10 MB file; report MB/s | C SDMMC driver via bindings |
| C: WiFi + HTTP | AP `wuw` serves `/jpg` to a phone; six parallel connections for 10 minutes with no heap growth, no reset | `esp_http_server` via bindings |

Common pieces: a `wuw-board` crate holds the pin constants once (mirrors
`docs/WIRING.md`); `tools/pincheck.py` is extended to check it so the Rust and
C++ pin maps cannot drift.

Test rig: one camera (CAM 01 or CAM 02, chosen by the owner), flashed over the
CH343 UART connector; MAC checked before flashing. The other camera keeps the
working firmware. The C++ firmware is restored with
`pio run -e wuwcam -t upload`.

## Phase 2 (after spikes, separate spec and plan)

- Per-peripheral decision: pure `no_std` (esp-hal + embassy) or ESP-IDF via
  `esp-idf-svc`, based on spike results.
- Module layout mirroring the C++ OS: camera, panel (ST7789 + touch), web
  server, recording (MJPEG in QuickTime to SD), WUW LINK, BWO game, PASAR.
- Real-time design: camera, encode and display pipeline on pinned cores with a
  fixed priority order; frame buffers in PSRAM; no per-frame heap allocation.
- Parity checklist against the C++ feature list and an A/B benchmark (`rgbbench`
  style) on the same camera before declaring the Rust build usable.

## Risks

- No confirmed `no_std` OV5640 DVP driver (spike A).
- No confirmed SDMMC host driver in esp-hal (spike B); SPI-mode SD may be
  impossible with the current wiring. To be checked against the board
  schematic.
- WiFi/HTTP crate maturity and API churn (spike C).
- Rust and C++ firmwares diverging; mitigated by the shared UI assets and the
  pin check.

## Testing

Spikes self-report. Phase 2 adds host-side unit tests for pure logic (parsers,
state machines, protocol framing) and on-device benchmarks recorded in
`docs/`.
