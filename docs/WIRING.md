# WUW CAM — Wiring Reference

**Board:** Freenove ESP32-S3-WROOM **N16R8** (16 MB flash, 8 MB octal PSRAM)
**Camera:** OV5640, 5 MP — on the board's own connector, pins fixed
**Display:** 2.4″ MSP2402 — measured ST7789 240×320 + HR2046 resistive touch
**Storage:** the board's onboard microSD

> Related: the Duck Life build uses the same module with **no camera**, which is
> why its display harness sits on GPIO 4–18. Those pins are the camera bus here.
> **The two pin maps are not interchangeable.**

---

## 1. Pins you cannot use

| Range | Claimed by | Consequence if used |
|---|---|---|
| `26–32` | SPI flash | chip cannot execute code |
| **`33–37`** | **octal PSRAM** | the R8 trap — free on R2 boards, dead on R8 |
| `43 / 44` | UART0 | this is how the board talks to you |
| `22–25` | — | do not exist on the S3 |
| `0, 3, 45, 46` | strapping | readable at reset; a module holding one at the wrong level stops the board booting |

## 2. Pins this build claims

| GPIO | Signal | Owner |
|---:|---|---|
| 4, 5 | SCCB SDA / SCL | camera |
| 6, 7 | VSYNC / HREF | camera |
| 8, 9, 10, 11, 12, 16, 17, 18 | D2, D1, D3, D0, D4, D7, D6, D5 | camera |
| 13 | PCLK | camera |
| 15 | XCLK | camera |
| 14 | shutter button | case |
| 38, 39, 40 | CMD / CLK / D0 | onboard microSD |
| 1, 2, 20, 21, 41, 42, 47, 48 | display + touch | panel |

Unclaimed after assembly: `0, 3, 19, 45, 46` — four are strapping pins and
GPIO19 is USB D−. GPIO20 (USB D+) is intentionally reassigned to TFT RESET;
use the CH343 UART connector for flashing. There is no pin left for audio.

Run `python3 tools/pincheck.py` to verify this against the source.

---

## 3. Display harness

### 3.1 Bridge three pairs at the module first

The LCD and the touch controller expose separate SPI pins but share one bus.
Tie each pair together **at the module**, then run one wire to the board.

| Bridge | Module pins | Single wire to |
|---|---|---|
| **B1** | `SCK` + `T_CLK` | GPIO 41 |
| **B2** | `SDI(MOSI)` + `T_DIN` | GPIO 2 |
| **B3** | `SDO(MISO)` + `T_DO` | GPIO 1 |

### 3.2 All fourteen module pins

Listed in physical order down the module header.

| # | Module pin | Goes to | Note |
|---:|---|---|---|
| 1 | `T_IRQ` | — | leave unconnected; firmware polls |
| 2 | `T_DO` | GPIO 1 | **B3** |
| 3 | `T_DIN` | GPIO 2 | **B2** |
| 4 | `T_CS` | GPIO 42 | touch chip select |
| 5 | `T_CLK` | GPIO 41 | **B1** |
| 6 | `SDO(MISO)` | GPIO 1 | **B3** |
| 7 | `LED` | GPIO 48 | backlight — **read §3.4 first** |
| 8 | `SCK` | GPIO 41 | **B1** |
| 9 | `SDI(MOSI)` | GPIO 2 | **B2** |
| 10 | `DC` | GPIO 47 | data/command |
| 11 | `RESET` | **GPIO 20** | deterministic hardware reset |
| 12 | `CS` | GPIO 21 | display chip select |
| 13 | `GND` | GND | |
| 14 | `VCC` | **3V3** | not 5 V |

The module's own `SD_SCK / SD_MISO / SD_MOSI / SD_CS` header is the display
board's SD slot. **Leave it unconnected** — this build uses the ESP32's.

### 3.3 Two ribbons, both on the right header

The seven shared-bus/control signals land on two contiguous runs. RESET uses a
separate GPIO20 lead:

```
   right header        module signal
     1   ──────────────  MISO   (B3)   ┐
     2   ──────────────  MOSI   (B2)   │  ribbon 1
    42   ──────────────  T_CS          │
    41   ──────────────  SCK    (B1)  ─┘

    48   ──────────────  LED           ┐
    47   ──────────────  DC            │  ribbon 2
    21   ──────────────  CS           ─┘

    20   ──────────────  RESET            separate lead
```

The six header pins between the runs are the microSD and the PSRAM, so one
unbroken run is impossible at any assignment. Two is the best available.

Bridge at the module and take each single wire off the **touch** pin of the
pair (2, 3, 5 — they sit together near the top). The shared-bus wiring remains
in the same order; RESET is the eighth signal lead.

`LED` is on GPIO 48 deliberately: that pin also feeds the onboard RGB LED on
these boards, so whatever sits there carries an extra stub. The backlight is on
or off and never toggles at speed, which makes it the cheapest thing to put
there. `T_CS` sits between `SCK` and `MOSI` for the same kind of reason — a
chip select idling high is good separation between a 40 MHz clock and its data
line, and the run has room for it at no cost.

### 3.4 ⚠️ Measure the backlight before driving it from GPIO 48

Four white LEDs at ~15 mA each is **~60 mA**, above the S3's 40 mA per-pin
maximum. Most MSP2402 boards have a driver transistor. Confirm:

1. Leave `LED` disconnected.
2. Multimeter in **current** mode between `3V3` and the module's `LED` pin.

| Reading | Meaning | Action |
|---|---|---|
| under 20 mA | driver transistor present | wire `LED` → GPIO 48 |
| 40 mA or more | no driver | 2N3904/BC337: base → GPIO 48 via 1 kΩ, collector → `LED`, emitter → GND |

For first bring-up you can tie `LED` straight to 3V3 — always on, always safe,
and it isolates the question to whether everything else works.

### 3.5 RESET goes to GPIO 20

The production firmware pulses GPIO 20 low during panel startup. Do not leave
RESET floating or tie it permanently to 3V3: the third unit demonstrated that
software reset alone does not recover every upload, brownout, or warm-restart
state. GPIO 20 is therefore reserved for the TFT reset line.

### 3.6 Keep leads under 10 cm

This jumper harness is verified clean at 10 MHz. Faster clocks produced torn,
duplicated, or neon-streaked rows without returning a software error, so the
production endpoint rejects values above 10 MHz and boot ignores stale faster
settings. Production also disables panel DMA and serializes touch and display
traffic through one SPI2 owner; the shared wires must never carry an LCD DMA
transfer while the HR2046 reader changes transaction speed. Test faster clocks
only with the `wuwcam-forcepanel` bench build after the display wiring is
replaced by a short soldered harness.

---

## 4. Shutter button

A 12×12 mm four-leg tactile switch. **GPIO 14 and GND** — both at the bottom of
the left header, adjacent, so it is a two-wire connector nowhere near the
display ribbon.

The four legs are two internally-joined pairs. Wire one leg and the
**diagonally opposite** one: diagonal legs are always in different pairs
whichever way the switch is turned, so the choice is orientation-proof.
Same-side legs are a permanent short and read as held-down-forever.

No resistor. The firmware uses `INPUT_PULLUP` and debounces in software.

| Action | Result |
|---|---|
| short press | photo |
| hold 3 s | start recording, fires **at** the 3 s mark |
| any press while recording | stop |

---

## 5. Bring-up

Flash and watch the serial log **before** building an enclosure:

```
pio run -e wuwcam -t upload && pio device monitor
```

Expected:

```
[PANEL] RDID4 0x00000000 -> no ID; this module never answers
[PANEL] ST7789 320x240, rotation 1, SPI 10 MHz; touch polls Z1
[PANEL] 320x240 up, skin NACRE
```

The controller ID is advisory only. This module does not answer RDID4, and its
ILI9341 silkscreen is wrong; the controller and colour settings were identified
by a rendered controller sweep. A missing ID must not disable the screen.

If it says `no panel answered`, the display is not run. Override only after
checking the wiring:

```bash
curl "http://<ip>/panel?k=wuw01&mode=on"     # run regardless of the probe
curl "http://<ip>/panel?k=wuw01&mode=auto"   # back to trusting it (default)
curl "http://<ip>/panel?k=wuw01&mode=off"    # never run
```

Applies on the next boot.

On a fresh install, the panel presents five yellow calibration targets. Press
each target firmly and lift fully between targets. A misplaced point is rejected
instead of being stored. To request another calibration on the next boot:

```bash
curl "http://<ip>/panel?k=wuw01&calib=1"
```

For isolated display/touch testing, use the retained standalone firmware:

```bash
pio run -d touch_diag -e touch-diag -t upload
```

---

## 6. Troubleshooting

| Symptom | Most likely cause |
|---|---|
| `no panel answered`, screen is wired | **B3 (MISO) bridge** — the only line nothing else exercises, so it is the usual culprit |
| Probe passes, panel dark | Backlight: `LED` unconnected, or needs a driver transistor (§3.4) |
| White screen, no image | Verify `DC` GPIO47, `CS` GPIO21, and `RESET` GPIO20 continuity |
| Torn, duplicated, or neon-streaked rows | SPI signal integrity, concurrent touch/LCD bus ownership, or a wrong decoded-row stride; production uses one SPI owner, synchronous LCD writes, a 320 px destination stride, and a 10 MHz cap |
| Touch reads garbage | `T_CS` shorted to `CS`, or B2/B3 crossed; run `touch_diag` |
| Touch is accurate in the test but dead in production | Request a new production calibration with `/panel?k=wuw01&calib=1`, then reboot |
| Camera fails to init after wiring the panel | A display lead on a camera pin. Run `tools/pincheck.py` |
| Nothing on serial | Wrong port — use **USB-UART**, not USB-OTG |

---

## 7. After changing the partition scheme

The board is configured for 16 MB (`default_16MB.csv`, 6.55 MB per OTA slot).
It was 8 MB until this change, and **the partition table moved**, so a plain
reflash boots to garbage. Erase first:

```bash
pio run -e wuwcam -t erase
pio run -e wuwcam -t upload
```

Erasing wipes NVS. These are stored there and must be set again afterwards:

```bash
curl "http://<ip>/name?k=wuw01&n=WUW-01"
curl "http://<ip>/net?k=wuw01&ssid=YOURWIFI&pass=YOURPASS"   # or MORE > WIFI on the glass
curl "http://<ip>/link/config?k=wuw01&group=wuw"
```

The PASAR session counter also resets, so the next session starts at `S0001`.
