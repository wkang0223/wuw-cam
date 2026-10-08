# View rotation, plain default picture, saving to the gallery, recording memory, WiFi password

Notes on one batch of changes to `main_s3.cpp` (the web page and its server), `video_writer.cpp`
and `panel.cpp`. Nothing here has been flashed yet; everything below says what was checked on a
computer and what still needs a real camera.

## 1. The phone view is turned the same way as the camera screen

The sensor is mounted so a portrait scene arrives sideways. The TFT has always turned it clockwise
(`VIEW ROTATE`, default 90 degrees); the phone page showed it unturned, 90 degrees to the left.

- One setting, `viewrot` (clockwise quarter turns, 0..3), lives in NVS namespace `wuw`, the same keys
  the panel uses (`viewrot`, `viewrotv`). Default 1. `GET /status` reports it, `GET /set?viewrot=N`
  changes it, and the panel follows at once (`panelSetViewRot`). The page re-syncs every 6 seconds.
- The page turns the picture with CSS on the `<img>` and the WebGL canvas, so nothing is re-encoded.
  A **VIEW ROTATE** button in the CAMERA section cycles 0/90/180/270.
- Saved media carries the same turn without touching pixels:
  - JPEG photos (SD, `/photo`, shared gallery frames) get an EXIF Orientation tag
    (`jpeg_orient.h`). Photos taken before this firmware have none and open as the sensor saw them.
  - Recordings carry it in the QuickTime track matrix (`rec_set_rotation`, latched at start).
  - Page-made PNG and MP4 files are drawn turned.
- Checked on a computer: EXIF read back by PIL; `.mov` read by ffprobe (`rotation=-90` for one
  clockwise turn, decoded frame 600x800 with the arrow upright); the real page in headless Chrome
  shows a sideways test frame upright at 90, and sideways/inverted at the other settings.

## 2. The default picture is untouched

The last render pass used to add film grain (slider default 15) and an S-curve to every frame, and
`body::after` drew a dark vignette over the whole page. Now the final pass is a plain copy unless an
effect, the exposure engine or the Grain slider is in use, grain defaults to 0, and the vignette is
gone. Measured in headless Chrome: background noise under 1 level, no overlay.

## 3. Colour

Sensor defaults were `brightness +2` and `ae_level +1`, which lifts every scene and washes highlights
toward pink. They are now 0 and 0 (a one-time reset on first boot, `camToneMigrate`); white balance
and saturation are remembered across reboots and shown correctly on the page.
**Not verified on a camera:** whether the remaining tint is the sensor's auto white balance. If a
photo still looks pink, try White Balance (IMAGE section) Sunny/Cloudy/Office and report which one is
closest; a grey-card calibration is the next step.

## 4. Saving to the gallery instead of the Files app

On an iPhone a file served as an attachment goes to the Files app. Served inline, Safari opens its own
viewer and the share sheet has Save Image / Save Video, which go straight into Photos.

- `/sd` (port 81) and `/dl` serve inline by default and answer HTTP byte ranges (`http_range.h`, 206),
  which Safari requires before it will play a video. `&dl=1` forces a download for Android/desktop.
- The page picks the right route per platform (SAVE buttons, SAVE TO GALLERY overlays).
- **Not verified on an iPhone:** video playback of the Motion-JPEG `.mov` and the Save Video step.

## 5. More memory for recording

`recModeEnter` (main_s3.cpp) gives back the 2 MB upload stash and the pre-roll ring when a clip starts
and restores them after; the recorder frees its effect ring and preview slots at stop; the ring budget
rose from 2 MB to 4 MB and 14 to 24 slots, sized from whatever PSRAM is free; the TFT preview drops to
about 10 fps without effects while recording, and the phone preview to about 8 fps. Free PSRAM is
logged on both transitions (`REC mode:` lines in the event log). Frame rate is still set by the
sensor mode (see the OV5640 notes); this removes the memory and bandwidth contention.
**Not measured on a camera.**

## 6. Your own WiFi password

`ap_pass.h` has the rules (8-63 printable ASCII; not the factory password, a repeated character or a
common one). `POST /ap/pass` takes `current\nnew`; it needs the `X-Wuw-Ap` header (so another website
cannot trigger it) and the current password (so a guest cannot lock the owner out). It is stored in
NVS key `appass`, applied about 1.5 s after the reply, and never returned. `GET /ap` only says whether
a custom password is set. Recovery: hold the shutter button (GPIO 14) at power-on for 8 seconds.
Not done: an on-device (touchscreen) editor for the password.

## Tests

```sh
g++ -std=c++17 -Wall -Wextra -I. tools/jpeg_orient_test.cpp -o /tmp/t && /tmp/t frame.jpg /tmp/out_
g++ -std=c++17 -Wall -Wextra -I. tools/http_range_test.cpp  -o /tmp/t && /tmp/t
g++ -std=c++17 -Wall -Wextra -I. tools/ap_pass_test.cpp     -o /tmp/t && /tmp/t
tools/rectest  (see its README; the last argument is the rotation)
WUW_TEST_JPG=sensor.jpg python3 tools/preview_server.py     # the real page against a fake camera
```
