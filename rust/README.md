# WUW CAM OS, Rust edition (alternative firmware)

An independent firmware for the same WUW CAM hardware. The C++ firmware at the
repo root is unchanged and remains the default. Pick one at flash time.

Status: Phase 1, feasibility spikes. Not a usable camera OS yet.

| Path | What |
|---|---|
| `wuw-board/` | Pin map (checked against the C++ map by `tools/pincheck.py`) |
| `wuw-frame/` | JPEG frame extraction from DMA buffers |
| `ov5640/` | OV5640 sensor driver |
| `spikes/` | Three on-device feasibility tests |
| `partitions-wuwcam.csv` | Same partition table as the C++ firmware, so settings survive switching |

Library crates are tested on the host:

    cd rust/wuw-board && cargo test

Spikes need the esp toolchain:

    source ~/export-esp.sh
    cd rust/spikes/spike-net && cargo run --release

Before flashing any camera for the first time, back up its flash:

    espflash read-flash 0 0x1000000 backup-<cam>-<date>.bin

Return to the C++ firmware with `pio run -e wuwcam -t upload` from the repo root.
