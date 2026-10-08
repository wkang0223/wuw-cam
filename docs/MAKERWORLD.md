# MakerWorld listing draft: WUW CAM case

Copy these sections into the MakerWorld upload form. Items marked **[you]** need
your own print, photos or slicing.

## Title
WUW CAM: ESP32-S3 touchscreen camera case (open source, with firmware)

## Summary (short description)
A two-part printable housing for an open-source ESP32-S3 + OV5640 camera with a
2.4" touchscreen. Firmware, wiring and build guide are on GitHub.

## Description
WUW CAM is an experimental camera built as an art object. It makes its own WiFi
network, shows a live view on visitors' phones with 60 WebGL effects, and saves
photos to a microSD card. This is its housing.

**Two shapes, each with a blank option**
- **Case** (`wuwcam_case_*`): clean CAD housing, 59-62 x 99 mm footprint.
- **Organic** (`wuwcam_organic_*`): sculpted pebble shape, 61-65 x 101 mm.
- **`_BLANK` files** are plain shells: no USB-C or microSD openings and no "w"
  relief on the back. Use them if you want to cut your own openings or remix.

Each variant is a front shell (screen side) and a back shell (camera side).
Back shell: 32 mm tall (case) / 30.5 mm (organic). Front shell: 15 mm / 16 mm.

**Assembly**
- Four M2 screws in the corner posts close the shell; heads are countersunk on
  the front face.
- A shutter-button seat sits in the chin under the screen (12x12 mm tactile
  switch, GPIO 14 to GND).
- USB-C (x2) openings on the bottom edge, microSD slot on the top edge.
- The housing is designed around the board's measured geometry, so the
  Freenove ESP32-S3 N16R8 + OV5640 board should drop in.

**Firmware and wiring:** https://github.com/wkang0223/wuw-cam (GPL-3.0). Start
with `docs/WIRING.md`. Models and docs are CC BY-SA 4.0.

## Bill of materials
| Qty | Part |
|---:|---|
| 1 | Freenove ESP32-S3-WROOM N16R8 board with OV5640 camera |
| 1 | 2.4" 240x320 SPI display module, ST7789 + HR2046 touch |
| 1 | microSD card |
| 1 | 12x12 mm four-leg tactile switch |
| 4 | M2 screws (length: **[you]** confirm with your print) |
| - | Thin hookup wire, short leads under 10 cm |

## Print settings **[you]**
Not yet tested on a Bambu printer. Suggested starting point, verify before you
publish:
- Material: PLA or PETG. Walls are 3 mm.
- Layer height 0.2 mm, 3+ walls, 15% infill.
- Orientation: shell openings face up (flat side down); supports may be needed
  for the port cutouts and the camera lens recess.
- Note the actual filament, time and weight from your slicer.

## Tags
esp32, esp32-s3, camera, ov5640, touchscreen, enclosure, case, open source,
arduino, platformio, art, diy electronics

## License
Creative Commons BY-SA 4.0 (MakerWorld: "Creative Commons - Attribution - Share Alike").

## Files to upload
From the GitHub Release `v0.1.0`: the eight `.stl` files and the `.step` file.
For best results export a `.3mf` from Bambu Studio with your print profile and
upload that as the main file **[you]**.

## Photos **[you]**
Printed case closed; open with the board installed; screen on; back with camera.
