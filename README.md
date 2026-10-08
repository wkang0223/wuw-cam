# WUW CAM

An experimental camera built as an art object. An ESP32-S3 with an OV5640 sensor
and a 2.4" touchscreen creates its own WiFi network. Visitors join it, watch the
live view on their phones, apply one of 60 WebGL effects, and take photographs
that stay on the device's microSD card.

The effects are fragment shaders that run on the **viewer's phone**, not on the
ESP32. The camera only serves JPEGs. Photographs never leave the card: there is
no outbound path for an image.

> Status: working hobby/art project, not a product. It is built for events where
> it is handed around a room, so the firmware favours recovery over cleverness.

## What is in this repo

| Path | Contents |
|---|---|
| `main_s3.cpp`, `panel.*`, `pasar.*`, `link.*`, `video_writer.*`, `bwo_game.*` | Firmware (Arduino framework on ESP32-S3, built with PlatformIO) |
| `docs/WIRING.md` | Pin map, display harness, shutter button, bring-up, troubleshooting |
| `case/` | Parametric 3D-printed housing (build123d + Blender scripts) |
| `assets/` | Skin borders and RPG tile art used by the UI |
| `site/` | Browser game preview and event page |
| `tools/` | `pincheck.py` (pin-conflict checker), art and font generators |
| `touch_diag/` | Standalone touchscreen diagnostics |

## Hardware

- Freenove ESP32-S3-WROOM **N16R8** board with OV5640 camera (16 MB flash, 8 MB octal PSRAM)
- 2.4" 240x320 SPI display module (ST7789) with HR2046 resistive touch
- microSD card (uses the board's onboard slot)
- 12x12 mm four-leg tactile switch for the shutter (GPIO 14 to GND)
- Hookup wire, and the printed case (see below)

Wiring, the three bridged SPI pairs, and the backlight caveat are in
[docs/WIRING.md](docs/WIRING.md). Run `python3 tools/pincheck.py` to check the
pin map against the source.

## Build and flash

Install [PlatformIO](https://platformio.org/), then:

```sh
pio run -e wuwcam                 # build
pio run -e wuwcam -t upload       # flash over the CH343 UART connector
pio device monitor
```

Use the CH343 UART port for flashing. GPIO 20 (USB D+) is reassigned to the TFT
reset on this build.

WiFi credentials are never stored in the source. Join the camera's own access
point (SSID `wuw`) or set a network from the on-device MORE > WIFI screen; it is
saved in the device's NVS.

**Set your own camera password.** A new camera joins with the factory password
`wuwuwuwu`, which is printed in the manual, so change it: open the page, then
CAMERA PASSWORD, enter the current and a new password (8 to 63 characters). It is
stored in NVS, applied at once (the camera's WiFi restarts, so rejoin with the new
one) and never sent back to the page. If it is forgotten, hold the shutter button
(GPIO 14) while powering on for 8 seconds to restore the factory password.
See [docs/VIEW_SAVING_PASSWORD.md](docs/VIEW_SAVING_PASSWORD.md).

## The case

`case/wuwcam_case.py` generates a two-part housing (front shell for the screen,
back shell for the camera) with 3 mm walls. Output files are not committed
because they are large and regenerable; the STL and STEP files are attached to
each GitHub Release.

| Files | What they are |
|---|---|
| `wuwcam_case_{front,back}.stl` | Clean CAD housing with USB-C, microSD and shutter openings |
| `wuwcam_organic_{front,back}.stl` | Sculpted pebble shape (Blender pass) |
| `*_BLANK.stl` | Plain shells without port openings or the "w" relief, for remixing |
| `wuwcam_case.step` | STEP file of the CAD housing |

Four M2 screws close the shell. See [docs/MAKERWORLD.md](docs/MAKERWORLD.md) for
bill of materials and print notes.

```sh
uvx --from build123d python case/wuwcam_case.py
"/Applications/Blender 3.app/Contents/MacOS/Blender" --background \
    --python case/blender_sculpt.py -- full
```

## Licenses

| Part | License |
|---|---|
| Firmware and tools | [GPL-3.0](LICENSE) |
| Case models, docs, artwork | [CC BY-SA 4.0](LICENSE-HARDWARE) |
| Panel font (Silkscreen) | SIL OFL 1.1, see `assets/fonts/OFL.txt` |

Third-party components and how the artwork was made are listed in [NOTICE](NOTICE).
