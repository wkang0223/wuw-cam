# Standalone touch diagnostic

This project is deliberately separate from the camera firmware. It exercises
only the ST7789 and XPT2046 on the shared SPI wiring.

Build and upload:

```sh
platformio run -d touch_diag -e touch-diag -t upload --upload-port /dev/cu.usbmodem1101
platformio device monitor -b 115200 --port /dev/cu.usbmodem1101
```

The test config holds DTR and RTS inactive. Native-USB monitors that toggle
those signals can reset an ESP32-S3 into ROM download mode mid-calibration.

Expected behavior:

- The ST7789 shows a black field, cyan border, and one yellow target at a time.
- Three top reference bars must appear red, green, then blue from left to right.
- Press and hold each yellow target until it turns green, then release before
  touching the next target.
- After five targets, a four-corner affine calibration corrects swapped axes,
  reversed axes, endpoint offsets, and mild panel skew. The four corners fit
  the map; the center point independently validates it.
- A missed, repeated, or misplaced target flashes the frame red and restarts
  at point one instead of accepting an inaccurate map.
- A green cursor then follows the calibrated touch position and the calculated
  coefficients are printed over serial.
- Repeated `MISO_STUCK` readings at all `0` or all `4095` mean the XPT2046 is
  not driving GPIO 1. Check B3 (`T_DO + SDO -> GPIO 1`) and T_CS on GPIO 42.

`T_IRQ` is intentionally unused. RESET must remain tied to 3V3.
